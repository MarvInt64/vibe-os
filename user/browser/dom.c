/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* dom — HTML parser + document tree. See dom.h. Syscall-free (umalloc only). */
#include "dom.h"
#include "umalloc.h"

/* ---- small char helpers ---- */
static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static int is_space(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static int is_name(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c==':'||c=='_'; }
static int ieq(const char *a,const char *b){ int i=0; for(;a[i]&&b[i];++i) if(lc(a[i])!=lc(b[i])) return 0; return a[i]==0&&b[i]==0; }
static int slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }

/* ---- arena ---- */
static char *arena_alloc(struct dom_doc *d, int len){
    char *p;
    if(d->arena_len + (unsigned long)len + 1 > d->arena_cap){
        unsigned long nc = d->arena_cap ? d->arena_cap*2 : 8192;
        char *na;
        while(nc < d->arena_len + (unsigned long)len + 1) nc*=2;
        na = (char*)urealloc(d->arena, nc);
        if(!na) return 0;
        d->arena = na; d->arena_cap = nc;
    }
    p = d->arena + d->arena_len;
    d->arena_len += (unsigned long)len + 1;
    return p;
}

static long arena_put(struct dom_doc *d, const char *s, int len){
    char *p = arena_alloc(d, len);
    long off;
    int i;
    if(!p) return -1;
    for(i=0;i<len;++i) p[i]=s[i];
    p[len]=0;
    off = (long)(p - d->arena);
    return off;
}

static struct dom_node *node_new(struct dom_doc *d, int type){
    struct dom_node *n = (struct dom_node*)umalloc(sizeof(struct dom_node));
    if(!n) return 0;
    if(d->node_count >= d->node_cap){
        int nc = d->node_cap ? d->node_cap*2 : 256;
        struct dom_node **nn = (struct dom_node**)urealloc(d->nodes, (umsize_t)nc*sizeof(struct dom_node*));
        if(!nn){ ufree(n); return 0; }
        d->nodes = nn; d->node_cap = nc;
    }
    d->nodes[d->node_count++] = n;
    n->type=type; n->tag[0]=0; n->text=0; n->attrs=0; n->nattrs=0;
    n->parent=n->first_child=n->last_child=n->next_sibling=0;
    return n;
}

static void node_append(struct dom_node *parent, struct dom_node *child){
    child->parent = parent;
    child->next_sibling = 0;
    if(parent->last_child){ parent->last_child->next_sibling = child; parent->last_child = child; }
    else { parent->first_child = parent->last_child = child; }
}

/* ---- entity + UTF-8 decode into the arena (returns offset) ---- */
static int decode_entity(const char *e,int n,char *out){
    char name[12]; int i;
    if(n<=0) return -1;
    if(e[0]=='#'){
        unsigned int cp=0; int j=1;
        if(j<n&&(e[j]=='x'||e[j]=='X')){ j++; for(;j<n;++j){ char c=lc(e[j]); if(c>='0'&&c<='9')cp=cp*16u+(unsigned)(c-'0'); else if(c>='a'&&c<='f')cp=cp*16u+(unsigned)(c-'a'+10); else return -1; } }
        else { for(;j<n;++j){ if(e[j]<'0'||e[j]>'9')return -1; cp=cp*10u+(unsigned)(e[j]-'0'); } }
        if(cp==160){ out[0]=' '; return 1; }
        if(cp<=0xFF){ out[0]=(char)cp; return 1; }
        if(cp==8211||cp==8212){ out[0]='-'; return 1; }
        if(cp==8216||cp==8217){ out[0]='\''; return 1; }
        if(cp==8220||cp==8221){ out[0]='"'; return 1; }
        out[0]='?'; return 1;
    }
    for(i=0;i<n&&i<11;++i) name[i]=e[i];
    name[i]=0;
    if(ieq(name,"amp")){ out[0]='&'; return 1; }
    if(ieq(name,"lt")){ out[0]='<'; return 1; }
    if(ieq(name,"gt")){ out[0]='>'; return 1; }
    if(ieq(name,"quot")){ out[0]='"'; return 1; }
    if(ieq(name,"apos")){ out[0]='\''; return 1; }
    if(ieq(name,"nbsp")){ out[0]=' '; return 1; }
    if(ieq(name,"mdash")||ieq(name,"ndash")){ out[0]='-'; return 1; }
    if(ieq(name,"hellip")){ out[0]='.'; out[1]='.'; out[2]='.'; return 3; }
    return -1;
}

/* Decode text run in[0..n): entities + UTF-8(->Latin-1).
 * Returns arena offset, sets *outlen. -1 on alloc failure. */
static long decode_text(struct dom_doc *d, const char *in, int n, int *outlen){
    char *buf = (char*)umalloc((umsize_t)n + 1);
    int i=0, o=0;
    long off;
    if(!buf){ *outlen=0; return -1; }
    while(i<n){
        char c=in[i];
        if(c=='&'){
            int j=i+1; while(j<n&&in[j]!=';'&&j-i<=10&&in[j]!='<'&&!is_space(in[j])) j++;
            if(j<n&&in[j]==';'){ char dec[4]; int dn=decode_entity(in+i+1,j-(i+1),dec);
                if(dn>0){ int q; for(q=0;q<dn;++q) buf[o++]=dec[q]; i=j+1; continue; } }
            buf[o++]='&'; i++; continue;
        }
        if((unsigned char)c >= 0x80){
            unsigned int cp; int len; unsigned char b=(unsigned char)c;
            if((b&0xE0)==0xC0 && i+1<n){ cp=((b&0x1Fu)<<6)|(in[i+1]&0x3Fu); len=2; }
            else if((b&0xF0)==0xE0 && i+2<n){ cp=((b&0x0Fu)<<12)|((in[i+1]&0x3Fu)<<6)|(in[i+2]&0x3Fu); len=3; }
            else if((b&0xF8)==0xF0 && i+3<n){ cp=((b&0x07u)<<18)|((in[i+1]&0x3Fu)<<12)|((in[i+2]&0x3Fu)<<6)|(in[i+3]&0x3Fu); len=4; }
            else { cp=b; len=1; }
            buf[o++] = (cp<=0xFFu)?(char)cp:'?';
            i+=len; continue;
        }
        buf[o++]=c; i++;
    }
    buf[o]=0;
    off = arena_put(d, buf, o);
    ufree(buf);
    *outlen = o;
    return off;
}

/* ---- void / raw element classification ---- */
static int tag_is_void(const char *t){
    static const char *v[]={"br","img","hr","meta","link","input","area","base","col","embed","source","track","wbr",0};
    int i; for(i=0;v[i];++i) if(ieq(t,v[i])) return 1; return 0;
}
static int tag_is_raw(const char *t){ return ieq(t,"script")||ieq(t,"style"); }

/* Auto-close: is `open` implicitly closed by a start tag `next`? (subset) */
static int implicitly_closes(const char *open, const char *next){
    if(ieq(open,"p")){
        static const char *b[]={"p","div","ul","ol","table","h1","h2","h3","h4","h5","h6",
            "blockquote","pre","section","article","header","footer","nav","form","hr","li",0};
        int i; for(i=0;b[i];++i) if(ieq(next,b[i])) return 1;
    }
    if(ieq(open,"li") && ieq(next,"li")) return 1;
    if((ieq(open,"td")||ieq(open,"th")) && (ieq(next,"td")||ieq(next,"th")||ieq(next,"tr"))) return 1;
    if(ieq(open,"tr") && ieq(next,"tr")) return 1;
    if((ieq(open,"dd")||ieq(open,"dt")) && (ieq(next,"dd")||ieq(next,"dt"))) return 1;
    if(ieq(open,"option") && (ieq(next,"option")||ieq(next,"optgroup"))) return 1;
    return 0;
}

void dom_init(struct dom_doc *d){
    d->arena=0; d->arena_len=d->arena_cap=0;
    d->nodes=0; d->node_count=d->node_cap=0;
    d->root=0;
}

void dom_free(struct dom_doc *d){
    int i;
    for(i=0;i<d->node_count;++i){
        if(d->nodes[i]->attrs) ufree(d->nodes[i]->attrs);
        ufree(d->nodes[i]);
    }
    if(d->nodes) ufree(d->nodes);
    if(d->arena) ufree(d->arena);
}

struct dom_node *dom_parse(struct dom_doc *d, const char *in, int n){
    struct dom_node *root, *cur;
    int i=0;

    root = node_new(d, DOM_ELEMENT);
    if(!root) return 0;
    { const char *t="#document"; int k=0; for(;t[k]&&k<23;++k) root->tag[k]=t[k]; root->tag[k]=0; }
    d->root = root;
    cur = root;

    while(i<n){
        if(in[i]=='<'){
            if(i+3<n && in[i+1]=='!' ){
                if(in[i+2]=='-'&&in[i+3]=='-'){ int j=i+4; while(j+2<n && !(in[j]=='-'&&in[j+1]=='-'&&in[j+2]=='>')) j++; i=(j+2<n)?j+3:n; continue; }
                { int j=i+2; while(j<n&&in[j]!='>') j++; i=(j<n)?j+1:n; continue; }
            }
            {
                int j=i+1, closing=0, k=0; char name[24];
                if(j<n&&in[j]=='/'){ closing=1; j++; }
                for(;j<n&&is_name(in[j])&&k<23;++j) name[k++]=lc(in[j]);
                name[k]=0;
                if(k==0){
                    int s=i; i++;
                    { int tl; long off=decode_text(d,in+s,1,&tl); if(off>=0&&tl>0){ struct dom_node *tn=node_new(d,DOM_TEXT); if(tn){ tn->text=(char*)(long)off; node_append(cur,tn);} } }
                    continue;
                }
                { char q=0; int end=j;
                  while(end<n){ char e=in[end]; if(q){ if(e==q)q=0; } else if(e=='"'||e=='\'')q=e; else if(e=='>')break; end++; }

                  if(closing){
                      struct dom_node *p=cur;
                      while(p && p!=root && !ieq(p->tag,name)) p=p->parent;
                      if(p && p!=root){ cur = p->parent ? p->parent : root; }
                      i=(end<n)?end+1:n;
                      continue;
                  }

                  while(cur!=root && implicitly_closes(cur->tag, name)) cur = cur->parent?cur->parent:root;

                  {
                      struct dom_node *el = node_new(d, DOM_ELEMENT);
                      int self_close = 0;
                      if(!el){ break; }
                      { int z=0; for(;name[z]&&z<23;++z) el->tag[z]=name[z]; el->tag[z]=0; }
                      node_append(cur, el);

                      {
                          struct dom_attr *list=0; int cnt=0, cap=0;
                          int a=j;
                          while(a<end){
                              int ns, ne; long noff, voff=-1;
                              while(a<end && (is_space(in[a])||in[a]=='/')) a++;
                              if(a>=end) break;
                              ns=a; while(a<end && is_name(in[a])) a++; ne=a;
                              if(ne==ns){ a++; continue; }
                              { char tmp[64]; int z=0,w; for(w=ns;w<ne&&z<63;++w) tmp[z++]=lc(in[w]); tmp[z]=0; noff=arena_put(d,tmp,z); }
                              while(a<end && is_space(in[a])) a++;
                              if(a<end && in[a]=='='){
                                  a++; while(a<end && is_space(in[a])) a++;
                                  if(a<end && (in[a]=='"'||in[a]=='\'')){ char qq=in[a]; int vs=++a; while(a<end && in[a]!=qq) a++; voff=arena_put(d,in+vs,a-vs); if(a<end)a++; }
                                  else { int vs=a; while(a<end && !is_space(in[a]) && in[a]!='>') a++; voff=arena_put(d,in+vs,a-vs); }
                              } else { voff=arena_put(d,"",0); }
                              if(cnt>=cap){ int ncap=cap?cap*2:4; struct dom_attr *nl=(struct dom_attr*)urealloc(list,(umsize_t)ncap*sizeof(struct dom_attr)); if(!nl)break; list=nl; cap=ncap; }
                              list[cnt].name=(char*)(long)noff; list[cnt].value=(char*)(long)voff; cnt++;
                          }
                          el->attrs=list; el->nattrs=cnt;
                      }

                      if(end>j && in[end-1]=='/') self_close=1;
                      i=(end<n)?end+1:n;

                      if(tag_is_raw(name)){
                          int s=i, nl=slen(name);
                          int e=i;
                          for(; e+1<n; ++e){ if(in[e]=='<'&&in[e+1]=='/'){ int m=0,p2=e+2; for(;m<nl&&p2<n;++m,++p2) if(lc(in[p2])!=lc(name[m]))break; if(m==nl){ break; } } }
                          if(e>s){ int tl; long off=decode_text(d,in+s,e-s,&tl); if(off>=0){ struct dom_node *tn=node_new(d,DOM_TEXT); if(tn){ tn->text=(char*)(long)off; node_append(el,tn);} } }
                          { int e2=e; while(e2<n&&in[e2]!='>')e2++; i=(e2<n)?e2+1:n; }
                          continue;
                      }

                      if(!self_close && !tag_is_void(name)) cur = el;
                  }
                }
            }
            continue;
        }
        {
            int s=i; while(i<n && in[i]!='<') i++;
            { int tl; long off=decode_text(d, in+s, i-s, &tl);
              if(off>=0 && tl>0){
                  int allspace=1, z; const char *txt=d->arena+off;
                  for(z=0;z<tl;++z) if(!is_space(txt[z])){ allspace=0; break; }
                  if(!allspace || ieq(cur->tag,"pre")){
                      struct dom_node *tn=node_new(d,DOM_TEXT);
                      if(tn){ tn->text=(char*)(long)off; node_append(cur,tn); }
                  }
              } }
        }
    }

    {
        int z;
        for(z=0;z<d->node_count;++z){
            struct dom_node *nd=d->nodes[z];
            if(nd->type==DOM_TEXT){ nd->text = d->arena + (long)nd->text; }
            else {
                int a;
                for(a=0;a<nd->nattrs;++a){
                    long noff=(long)nd->attrs[a].name, voff=(long)nd->attrs[a].value;
                    nd->attrs[a].name = d->arena + noff;
                    nd->attrs[a].value = d->arena + voff;
                }
            }
        }
    }

    return root;
}

const char *dom_attr(const struct dom_node *n, const char *name){
    int i;
    for(i=0;i<n->nattrs;++i) if(ieq(n->attrs[i].name,name)) return n->attrs[i].value;
    return 0;
}

/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/* webtext — HTML-to-readable-text + word wrap. See webtext.h. */
#include "webtext.h"
#include "umalloc.h"

static int slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }
static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static int is_space(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static int is_alnum(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'); }
static int ieq(const char *a,const char *b){ int i=0; for(;a[i]&&b[i];++i) if(lc(a[i])!=lc(b[i])) return 0; return a[i]==0&&b[i]==0; }

/* Decode an entity body (between '&' and ';') into up to 3 chars. -1 = unknown. */
static int decode_entity(const char *e,int n,char *out){
    char name[12]; int i;
    if(n<=0) return -1;
    if(e[0]=='#'){
        unsigned int cp=0; int j=1;
        if(j<n&&(e[j]=='x'||e[j]=='X')){ j++; for(;j<n;++j){ char c=lc(e[j]); if(c>='0'&&c<='9')cp=cp*16u+(unsigned)(c-'0'); else if(c>='a'&&c<='f')cp=cp*16u+(unsigned)(c-'a'+10); else return -1; } }
        else { for(;j<n;++j){ if(e[j]<'0'||e[j]>'9')return -1; cp=cp*10u+(unsigned)(e[j]-'0'); } }
        if(cp==160){ out[0]=' '; return 1; }
        if(cp<128){ out[0]=(char)cp; return 1; }
        if(cp==8211||cp==8212){ out[0]='-'; return 1; }
        if(cp==8216||cp==8217){ out[0]='\''; return 1; }
        if(cp==8220||cp==8221){ out[0]='"'; return 1; }
        out[0]='?'; return 1;
    }
    for(i=0;i<n&&i<11;++i) name[i]=e[i];
    name[i]=0;
    if(ieq(name,"amp")) { out[0]='&'; return 1; }
    if(ieq(name,"lt"))  { out[0]='<'; return 1; }
    if(ieq(name,"gt"))  { out[0]='>'; return 1; }
    if(ieq(name,"quot")){ out[0]='"'; return 1; }
    if(ieq(name,"apos")){ out[0]='\''; return 1; }
    if(ieq(name,"nbsp")){ out[0]=' '; return 1; }
    if(ieq(name,"mdash")||ieq(name,"ndash")){ out[0]='-'; return 1; }
    if(ieq(name,"copy")){ out[0]='('; out[1]='c'; out[2]=')'; return 3; }
    if(ieq(name,"hellip")){ out[0]='.'; out[1]='.'; out[2]='.'; return 3; }
    return -1;
}

static int tag_is_break(const char *name){
    static const char *blk[]={"p","br","div","li","tr","ul","ol","table","h1","h2","h3",
        "h4","h5","h6","blockquote","hr","section","article","header","footer","nav",
        "pre","form","figure","figcaption","dd","dt","dl","aside","main",0};
    int i; for(i=0;blk[i];++i) if(ieq(name,blk[i])) return 1; return 0;
}

static int skip_until_close(const char *in,int n,int start,const char *name){
    int i=start, nl=slen(name);
    for(;i+1<n;++i){
        if(in[i]=='<'&&in[i+1]=='/'){
            int k=0,j=i+2;
            for(;k<nl&&j<n;++k,++j) if(lc(in[j])!=lc(name[k])) break;
            if(k==nl){ while(j<n&&in[j]!='>')++j; return (j<n)?j+1:n; }
        }
    }
    return n;
}

/* Append a single '\n' break, trimming a trailing space and collapsing runs. */
static int emit_break(char *out,int o){
    if(o>0&&out[o-1]==' ')o--;
    if(o>0&&out[o-1]!='\n') out[o++]='\n';
    return o;
}

void webtext_init(struct webtext *wt){
    wt->text=0; wt->text_len=0; wt->lines=0; wt->line_count=0; wt->line_cap=0;
}

void webtext_free(struct webtext *wt){
    if(wt->text){ ufree(wt->text); }
    if(wt->lines){ ufree(wt->lines); }
    webtext_init(wt);
}

int webtext_from_html(struct webtext *wt,const char *in,int n){
    char *out;
    int i=0,o=0;
    if(wt->text){ ufree(wt->text); wt->text=0; wt->text_len=0; }
    if(n<0) n=0;
    out=(char*)umalloc((umsize_t)n+1);
    if(!out) return -1;

    while(i<n){
        char c=in[i];
        if(c=='<'){
            int j=i+1,closing=0,k=0,end; char name[16];
            if(j<n&&in[j]=='/'){ closing=1; j++; }
            for(;j<n&&is_alnum(in[j])&&k<15;++j) name[k++]=in[j];
            name[k]=0;
            end=j; while(end<n&&in[end]!='>')++end;
            if(!closing&&(ieq(name,"script")||ieq(name,"style")||ieq(name,"head"))){
                i=skip_until_close(in,n,(end<n)?end+1:n,name);
                o=emit_break(out,o);
                continue;
            }
            if(tag_is_break(name)) o=emit_break(out,o);
            i=(end<n)?end+1:n;
            continue;
        }
        if(c=='&'){
            int j=i+1; while(j<n&&in[j]!=';'&&j-i<=10&&in[j]!='<'&&!is_space(in[j]))++j;
            if(j<n&&in[j]==';'){
                char dec[4]; int dn=decode_entity(in+i+1,j-(i+1),dec);
                if(dn>0){ int d; for(d=0;d<dn;++d){ char dc=dec[d];
                    if(is_space(dc)){ if(o>0&&out[o-1]!=' '&&out[o-1]!='\n')out[o++]=' '; }
                    else out[o++]=dc; }
                    i=j+1; continue; }
            }
            out[o++]='&'; i++; continue;
        }
        if(is_space(c)){
            if(o>0&&out[o-1]!=' '&&out[o-1]!='\n') out[o++]=' ';
            i++; continue;
        }
        out[o++]=c; i++;
    }
    while(o>0&&(out[o-1]==' '||out[o-1]=='\n'))o--;
    out[o]=0;
    wt->text=out; wt->text_len=o;
    return o;
}

static void push_line(struct webtext *wt,int off,int len){
    if(wt->line_count>=wt->line_cap){
        int nc=wt->line_cap? wt->line_cap*2 : 128;
        struct webline *nl=(struct webline*)urealloc(wt->lines,(umsize_t)nc*sizeof(struct webline));
        if(!nl) return;
        wt->lines=nl; wt->line_cap=nc;
    }
    wt->lines[wt->line_count].off=off;
    wt->lines[wt->line_count].len=len;
    wt->line_count++;
}

void webtext_wrap(struct webtext *wt,int cols){
    int ls=0;
    wt->line_count=0;
    if(cols<8) cols=8;
    if(!wt->text){ return; }
    while(ls<wt->text_len){
        int end=ls, cut=-1;
        while(end<wt->text_len && wt->text[end]!='\n' && (end-ls)<cols){
            if(wt->text[end]==' ') cut=end-ls;
            end++;
        }
        if(end<wt->text_len && wt->text[end]!='\n' && (end-ls)==cols){
            if(cut>0){ push_line(wt,ls,cut); ls=ls+cut+1; }
            else { push_line(wt,ls,cols); ls=ls+cols; }
        } else {
            push_line(wt,ls,end-ls);
            if(end<wt->text_len && wt->text[end]=='\n') ls=end+1;
            else ls=end;
        }
    }
    if(wt->line_count==0) push_line(wt,0,0);
}

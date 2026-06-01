#pragma once

/* Fixed-capacity string builder that lives entirely on the stack.
 * Avoids heap allocation for the short, frequently-rebuilt labels in the
 * task manager UI.  Cap includes the NUL terminator. */
template <int Cap>
class StringBuilder {
public:
    StringBuilder() : len_(0) { buf_[0] = '\0'; }

    /* Append a NUL-terminated string. Silently truncates at capacity. */
    StringBuilder &append(const char *s) {
        while (s && *s && len_ + 1 < Cap)
            buf_[len_++] = *s++;
        buf_[len_] = '\0';
        return *this;
    }

    /* Append an unsigned integer in decimal. */
    StringBuilder &append(unsigned long v) {
        if (v == 0) return append("0");
        char tmp[22];
        int n = 0;
        while (v && n < (int)sizeof(tmp)) { tmp[n++] = '0' + (int)(v % 10); v /= 10; }
        while (n--) { if (len_ + 1 < Cap) buf_[len_++] = tmp[n + 1]; }
        buf_[len_] = '\0';
        return *this;
    }

    StringBuilder &append(int v) {
        if (v < 0) { append("-"); v = -v; }
        return append(static_cast<unsigned long>(v));
    }

    StringBuilder &append(unsigned int v) { return append(static_cast<unsigned long>(v)); }

    /* Append a human-readable byte size: "512 B", "12.3 KB", "8.0 MB". */
    StringBuilder &append_size(unsigned long bytes) {
        if (bytes < 1024UL) { append(bytes); return append(" B"); }
        unsigned long divisor = (bytes < 1024UL * 1024UL) ? 1024UL : 1024UL * 1024UL;
        const char *unit      = (bytes < 1024UL * 1024UL) ? " KB" : " MB";
        unsigned long x10 = bytes * 10UL / divisor;   /* value × 10 for one decimal */
        append(x10 / 10UL).append(".").append(x10 % 10UL);
        return append(unit);
    }

    void clear() { len_ = 0; buf_[0] = '\0'; }

    const char *c_str() const { return buf_; }

private:
    char buf_[Cap];
    int  len_;
};

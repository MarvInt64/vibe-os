/* Small kernel-side C++ smoke object.
 *
 * This is intentionally tiny: it verifies that .init_array constructors run in
 * the kernel without requiring dynamic allocation or touching device state.
 */
class KernelCxxSmoke {
public:
    KernelCxxSmoke() : value_(42) {}
    int value() const { return value_; }

private:
    int value_;
};

static KernelCxxSmoke g_kernel_cxx_smoke;

extern "C" int kernel_cxx_smoke_value(void) {
    return g_kernel_cxx_smoke.value();
}

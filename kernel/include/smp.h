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

/*
 * SMP bring-up (milestone 2): start the application processors.
 *
 * Uses the ACPI CPU list and the Local APIC to INIT-SIPI-SIPI every non-boot
 * CPU through the real-mode trampoline in ap_boot.S. For now each AP just
 * enables its Local APIC and parks in a halt loop; the scheduler still runs
 * only on the boot CPU. The point of this milestone is purely to prove the
 * other cores come online (see smp_cpu_count).
 */
#ifndef VIBEOS_SMP_H
#define VIBEOS_SMP_H

/* Start every application processor. No-op without an active APIC. Must run
 * after apic_init() and after the kernel heap is available (AP stacks). */
void smp_boot_aps(void);

/* Number of CPUs that have come online (the boot CPU counts as one). */
unsigned smp_cpu_count(void);

#endif

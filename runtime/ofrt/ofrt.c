/* libofrt - the OptiForge language runtime.
 *
 * Linked into every compiled program. Deliberately plain C with no dependency
 * on any compiler header: this is built for the TARGET, not the host
 * (architectural_design.md section 3, rule 7).
 *
 * The generated `main` is the C `main`, so the platform CRT performs startup
 * and no entry shim is needed.
 */

#include <stdio.h>

void print_int(long long value) { printf("%lld\n", value); }

void print_float(double value) { printf("%g\n", value); }

void print_bool(int value) { printf("%s\n", value ? "true" : "false"); }

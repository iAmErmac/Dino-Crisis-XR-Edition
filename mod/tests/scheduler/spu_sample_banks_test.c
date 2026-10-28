#include "test_process.h"
#define main spu_sample_banks_contract_main
#include "test_spu_sample_banks.c"
#undef main
#include <stdlib.h>
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }
int main(void) { test_process_init(); return spu_sample_banks_contract_main(); }

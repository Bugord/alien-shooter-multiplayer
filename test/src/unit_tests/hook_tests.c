#include "../eztest/eztest.h"
#include "ehook.h"

typedef int (*sum_t)(int, int);
static sum_t orig_caller_;

__attribute__((__naked__)) int sum_naked(int a, int b)
{
    (void)a;
    (void)b;
    __asm__("nop");
    __asm__("nop");
    __asm__("nop");
    __asm__("nop");
    __asm__("nop");
    __asm__("pushl %ebp");
    __asm__("movl %esp,%ebp");
    __asm__("movl 8(%ebp),%eax");
    __asm__("addl 12(%ebp),%eax");
    __asm__("popl %ebp");
    __asm__("ret");
}

int sum_hook(int a, int b)
{
    a += 1;
    b += 2;
    return orig_caller_(a, b);
}

TEST_BEGIN(test_01)
{
    EXPECT(sum_naked(4, 3), 7);
    orig_caller_ = (sum_t)eh_set_trampoline_hook(sum_naked, sum_hook, 5,
                                                 EH_TT_TRAMPOLINE_JMP);
    EXPECT_NOT_ZERO(orig_caller_);
    EXPECT(sum_naked(4, 3), 10);
    eh_unset_trampoline_hook(sum_naked, orig_caller_, 5, EH_TT_TRAMPOLINE_JMP);
    EXPECT(sum_naked(4, 3), 7);
    orig_caller_ = (sum_t)eh_set_trampoline_hook(sum_naked, sum_hook, 5,
                                                 EH_TT_TRAMPOLINE_JMP);
    EXPECT_NOT_ZERO(orig_caller_);
    EXPECT(sum_naked(4, 3), 10);
    eh_unset_trampoline_hook(sum_naked, orig_caller_, 5, EH_TT_TRAMPOLINE_JMP);
    EXPECT(sum_naked(4, 3), 7);
}
TEST_END

RUN_TESTS(FILENAME, test_01);

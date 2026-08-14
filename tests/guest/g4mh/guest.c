/* A G4MH guest built by CC-RH, exercising integer and FP paths. */

#pragma inline_asm sys3
static int sys3(int nr, int a0, const void *a1, int a2)
{
    mov r6, r11        ; nr
    mov r7, r6         ; arg0
    mov r8, r7         ; arg1
    mov r9, r8         ; arg2
    trap 0
}

static void wr(const char *s, int n)
{
    (void)sys3(64, 1, s, n);
}

static void puts_(const char *s)
{
    int n = 0;
    while (s[n]) { n++; }
    wr(s, n);
}

static void puthex(unsigned v)
{
    char b[11];
    int i;
    unsigned d;

    b[0] = '0';
    b[1] = 'x';
    for (i = 0; i < 8; i++) {
        d = (v >> ((7 - i) * 4)) & 0xFu;
        b[2 + i] = (char)(d < 10u ? '0' + d : 'a' + d - 10u);
    }
    b[10] = '\n';
    wr(b, 11);
}

static int arr[8];

int main(void)
{
    unsigned checks = 0, fails = 0;
    volatile int a = 7, b = -3;
    volatile float x = 2.0f, y = 0.5f;
    int i;

    puts_("BANNER\n");
    checks++; if (a + b != 4)              { fails++; }
    checks++; if (a * b != -21)            { fails++; }
    checks++; if (a / b != -2)             { fails++; }
    checks++; if ((unsigned)a % 3u != 1u)  { fails++; }
    checks++; if ((a << 3) != 56)          { fails++; }
    checks++; if ((b >> 1) != -2)          { fails++; }

    puts_("INT-OK\n");
    checks++; if (x + y != 2.5f)           { fails++; }
    checks++; if (x - y != 1.5f)           { fails++; }
    checks++; if (x * y != 1.0f)           { fails++; }
    checks++; if (x / y != 4.0f)           { fails++; }
    checks++; if ((int)(x * 3.0f) != 6)    { fails++; }
    checks++; if ((float)(int)7 != 7.0f)   { fails++; }
    checks++; if (!(y < x))                { fails++; }
    checks++; if (y > x)                   { fails++; }

    puts_("FP-OK\n"); puthex(fails);
    for (i = 0; i < 8; i++) { arr[i] = i * i; }
    puts_("ARR-OK\n");
    checks++; if (arr[7] != 49)            { fails++; }
    puts_("LOOP-OK\n");

    puts_("G4MH-GUEST checks=");
    puthex(checks);
    puts_("G4MH-GUEST fails=");
    puthex(fails);
    puts_(fails ? "FAIL\n" : "PASS\n");
    return (int)fails;
}

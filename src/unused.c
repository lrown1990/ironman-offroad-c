/* The routines of game.exe the game never calls (map_game.py, DEAD_CODE):
 * helpers of the library and of the game that were left in, as
 * tools/unused.py has them. Game segment, near routines:
 *   0f28  BX = table word: CX = [2faa + 2*AL], BX = [2faa + (2*AL + 80h) & 1ffh]
 *   3936  AX (binary) to four BCD digits     3951  four BCD digits to binary
 *   397a  four BCD digits to four ASCII      3994  four ASCII digits to BCD
 *   39a8  four hexadecimal digits to ASCII   39e0  four hexadecimal ASCII to the word
 *   3a12  binary to ASCII (3936, then 397a)  3a19  ASCII to binary (3994, then 3951)
 * The registers each leaves are in Regs5 (only those the model gives). */
#include "offroad.h"

void unused_table_pair(const uint8_t *m, unsigned al, Regs5 *r) {   /* 0f28 */
    unsigned bx = (al & 255) * 2;
    r->cx = (uint16_t)(m[(0x2faa + bx) & 0xffff] | m[(0x2fab + bx) & 0xffff] << 8);
    bx = (bx + 0x80) & 0x1ff;
    r->bx = (uint16_t)(m[(0x2faa + bx) & 0xffff] | m[(0x2fab + bx) & 0xffff] << 8);
}
/* 3936: four divisions by 10, each remainder rotated in at the top of BP */
void unused_binary_to_bcd(unsigned ax, Regs5 *r) {
    unsigned bp = 0, rem = 0;
    for (int i = 0; i < 4; i++) {
        rem = ax % 10; ax /= 10;
        bp = (bp + rem) & 0xffff; bp = ((bp >> 4) | (bp << 12)) & 0xffff;
    }
    r->ax = (uint16_t)bp; r->bx = 10; r->cx = 0; r->dx = (uint16_t)rem; r->bp = (uint16_t)bp;
}
void unused_bcd_to_binary(unsigned ax, Regs5 *r) {   /* 3951 */
    unsigned cl = ax & 15, al = (ax & 255) ^ cl;
    al >>= 1; cl = (cl + al) & 255; al >>= 2; cl = (cl + al) & 255;
    unsigned ch = (ax >> 8) & 15, ah = ((ax >> 8) & 255) ^ ch;
    ah >>= 1; ch = (ch + ah) & 255; ah >>= 2; ah = (ah + ch) & 255;
    r->ax = (uint16_t)(100 * ah + cl); r->cx = (uint16_t)cl;
}
void unused_bcd_to_ascii(unsigned ax, Regs5 *r) {   /* 397a */
    unsigned cx = ax & 0x0f0f;
    ax = ((ax ^ cx) >> 4) & 0xffff;
    ax = (ax + 0x3030) & 0xffff; cx = (cx + 0x3030) & 0xffff;
    unsigned al = cx >> 8, ch = ax & 255;
    r->ax = (uint16_t)((ax & 0xff00) | al); r->cx = (uint16_t)((ch << 8) | (cx & 255));
}
void unused_ascii_to_bcd(unsigned ax, unsigned cx, Regs5 *r) {   /* 3994 */
    unsigned al = cx >> 8, ch = ax & 255;
    ax = (ax & 0xff00) | al; cx = (ch << 8) | (cx & 255);
    ax = (ax - 0x3030) & 0xffff; cx = (cx - 0x3030) & 0xffff;
    ax = (ax << 4) & 0xffff;
    r->ax = (uint16_t)(ax | cx); r->cx = (uint16_t)cx;
}
static unsigned hex_up(unsigned v) { return v >= 10 ? (v + 0x27) & 255 : v; }
static unsigned hex_down(unsigned v) { return v >= 10 ? (v - 0x27) & 255 : v; }
void unused_hex_to_ascii(unsigned ax, Regs5 *r) {   /* 39a8 */
    unsigned cx = ax & 0x0f0f;
    ax = ((ax ^ cx) >> 4) & 0xffff;
    unsigned ch = hex_up(cx >> 8), cl = hex_up(cx & 255), ah = hex_up(ax >> 8), al = hex_up(ax & 255);
    ax = ((ah << 8 | al) + 0x3030) & 0xffff; cx = ((ch << 8 | cl) + 0x3030) & 0xffff;
    al = cx >> 8; ch = ax & 255;
    r->ax = (uint16_t)((ax & 0xff00) | al); r->cx = (uint16_t)((ch << 8) | (cx & 255));
}
void unused_ascii_to_hex(unsigned ax, unsigned cx, Regs5 *r) {   /* 39e0 */
    unsigned al = cx >> 8, ch = ax & 255;
    ax = (ax & 0xff00) | al; cx = (ch << 8) | (cx & 255);
    ax = (ax - 0x3030) & 0xffff; cx = (cx - 0x3030) & 0xffff;
    ch = hex_down(cx >> 8); unsigned cl = hex_down(cx & 255), ah = hex_down(ax >> 8);
    al = hex_down(ax & 255);
    ax = ((ah << 8 | al) << 4) & 0xffff; cx = ch << 8 | cl;
    r->ax = (uint16_t)(ax | cx); r->cx = (uint16_t)cx;
}
void unused_binary_to_ascii(unsigned ax, Regs5 *r) {   /* 3a12 */
    unused_binary_to_bcd(ax, r);
    Regs5 out;
    unused_bcd_to_ascii(r->ax, &out);
    r->ax = out.ax; r->cx = out.cx;
}
void unused_ascii_to_binary(unsigned ax, unsigned cx, Regs5 *r) {   /* 3a19 */
    unused_ascii_to_bcd(ax, cx, r);
    Regs5 out;
    unused_bcd_to_binary(r->ax, &out);
    r->ax = out.ax; r->cx = out.cx;
}

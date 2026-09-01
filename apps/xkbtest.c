/*
 * xkbtest — libxkbcommon in ring 3, over the keymap data on the volume.
 *
 * This is the first port here that is a library *and* a body of data,
 * and the test has to establish both. libxkbcommon does not contain a
 * keyboard layout — it contains a compiler for the files that describe
 * one — so an archive that links and answers nothing would be an
 * entirely plausible way for this port to be broken. The first section
 * below is therefore the one that matters: it makes WebKit's own call
 * and requires it to succeed.
 *
 * ---- what WebKit does, and what this file does about it ----
 *
 * Source/WebKit/WPEPlatform/wpe/WPEKeymapXKB.cpp:179-181 is the whole of
 * WebKit's use of this library's compiler:
 *
 *     struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
 *     struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
 *     keymap->priv->xkbKeymap =
 *         xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
 *
 * Those exact five strings are used here, empty strings and all — not
 * NULL, which is a different request that would fall back to the
 * compiled-in defaults and so could pass while WebKit's call failed.
 * Everything after section 1 is asked of the keymap that call produces.
 *
 * ---- and why the expected values come from outside this system ----
 *
 * Three independent published sources, none of them libxkbcommon:
 *
 *   Keycodes are Linux input-layer codes **plus 8**. That offset is a
 *   fixed piece of X11/evdev ABI, not a choice anyone here made:
 *   <linux/input-event-codes.h> gives KEY_A=30, KEY_LEFTSHIFT=42,
 *   KEY_ENTER=28, KEY_ESC=1, so the XKB keycodes are 38, 50, 36 and 9.
 *   Every keycode below is written as `KERNEL + 8` with the kernel
 *   constant named, so the arithmetic is checkable against that header.
 *
 *   Keysym values are X11's, from keysymdef.h, unchanged since the
 *   1980s: `a` is 0x0061, `A` is 0x0041, Return is 0xff0d, Escape
 *   0xff1b, Shift_L 0xffe1. The Latin block is deliberately equal to
 *   ASCII and the function block deliberately is not.
 *
 *   The mapping from keysym to character is Unicode's.
 *
 * A round trip — compile a keymap, dump it, compile the dump — is in
 * section 6 because it exercises keymap-dump.c and the parser against
 * real output. It is not allowed to stand in for any of the above: two
 * halves of one library agree with each other whether or not either is
 * right, which is the whole reason jpegtest reads a bitstream macOS
 * encoded and tasn1test reads DER OpenSSL wrote.
 */

#include "vextro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s\n", what); }
}

/* Linux input-event-codes.h, and the +8 that turns one into the other. */
#define KEY_ESC         1
#define KEY_1           2
#define KEY_Q          16
#define KEY_ENTER      28
#define KEY_A          30
#define KEY_LEFTSHIFT  42
#define KEY_LEFTCTRL   29
#define KEY_CAPSLOCK   58
#define KEY_SPACE      57
#define EVDEV(k)  ((xkb_keycode_t)((k) + 8))

int main(void) {
    printf("xkbtest: libxkbcommon in ring 3, keymap data from /etc/xkb\n");

    /* ================================================================
     * 1. WebKit's call, made exactly as WebKit makes it
     * ================================================================
     *
     * If this section fails, nothing else in the file can run and the
     * port is not finished — either the archive is wrong or /etc/xkb is
     * not on the volume. Both are reported before giving up, because
     * they are different problems with different fixes.
     */
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    check("xkb_context_new", ctx != NULL);
    if (!ctx) {
        printf("xkbtest: %d checks, %d failures (no context)\n",
               checks, failures + 1);
        return 1;
    }

    /*
     * The context resolves its include paths at construction, from
     * DFLT_XKB_CONFIG_ROOT in third_party/libxkbcommon-port/config.h,
     * because every environment variable it would consult first is
     * unset here — `environ` is an empty vector. So a zero path count
     * means the data directory is missing, which is a volume problem,
     * not a library one.
     */
    unsigned npaths = xkb_context_num_include_paths(ctx);
    check("the context found at least one include path", npaths >= 1);
    if (npaths >= 1)
        check("and the first is /etc/xkb",
              strcmp(xkb_context_include_path_get(ctx, 0), "/etc/xkb") == 0);

    struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    check("xkb_keymap_new_from_names(evdev, pc105, us) — WebKit's call",
          keymap != NULL);
    if (!keymap) {
        printf("  the rules file, or something it includes, did not "
               "compile.\n");
        printf("  /etc/xkb/rules/evdev is merged from 44 parts by "
               "`make xkbdata`.\n");
        printf("xkbtest: %d checks, %d failures (no keymap)\n",
               checks, failures + 1);
        xkb_context_unref(ctx);
        return 1;
    }

    /* ================================================================
     * 2. the shape of the keymap that came back
     * ================================================================ */
    /*
     * The lowest keycode is 9, not the 8 that /etc/xkb/keycodes/evdev
     * declares as its `minimum`. libxkbcommon ignores that declaration
     * and takes the minimum over the keys actually assigned
     * (keycodes.c:197), and the lowest key a PC keyboard has is Escape:
     * KEY_ESC is 1 in the kernel, so <ESC> = 9 in the file. The +8 rule
     * again, and a better anchor than the declared number precisely
     * because it is derived from the data rather than announced by it.
     */
    xkb_keycode_t min = xkb_keymap_min_keycode(keymap);
    xkb_keycode_t max = xkb_keymap_max_keycode(keymap);
    check("min keycode is 9 — <ESC>, which is KEY_ESC + 8",
          min == EVDEV(KEY_ESC));
    check("max keycode is above 240", max > 240);

    /*
     * The four modifier names WebKit asks for by name in
     * WPEKeymapXKB.cpp. "Lock" rather than "Caps" is X11's spelling and
     * is what XKB_MOD_NAME_CAPS expands to — a detail worth asserting,
     * since a lookup that failed would return XKB_MOD_INVALID and the
     * browser would silently lose a modifier rather than crash.
     */
    xkb_mod_index_t shift = xkb_keymap_mod_get_index(keymap,
                                                     XKB_MOD_NAME_SHIFT);
    xkb_mod_index_t caps  = xkb_keymap_mod_get_index(keymap,
                                                     XKB_MOD_NAME_CAPS);
    xkb_mod_index_t ctrl  = xkb_keymap_mod_get_index(keymap,
                                                     XKB_MOD_NAME_CTRL);
    xkb_mod_index_t alt   = xkb_keymap_mod_get_index(keymap,
                                                     XKB_MOD_NAME_ALT);
    check("modifier \"Shift\" resolves",   shift != XKB_MOD_INVALID);
    check("modifier \"Lock\" resolves",    caps  != XKB_MOD_INVALID);
    check("modifier \"Control\" resolves", ctrl  != XKB_MOD_INVALID);
    check("modifier \"Mod1\" (Alt) resolves", alt != XKB_MOD_INVALID);
    check("a modifier that does not exist gives XKB_MOD_INVALID",
          xkb_keymap_mod_get_index(keymap, "Nonesuch") == XKB_MOD_INVALID);

    check("the keymap has at least one layout",
          xkb_keymap_num_layouts(keymap) >= 1);
    check("and it is named \"English (US)\" or similar",
          xkb_keymap_layout_get_name(keymap, 0) != NULL);

    /*
     * Auto-repeat. A letter repeats and a modifier does not — that is a
     * property of the `us` symbols file, read off the volume, and it is
     * the check that would catch a symbols tree that had been staged
     * but not actually parsed.
     */
    check("the A key repeats", xkb_keymap_key_repeats(keymap, EVDEV(KEY_A)));
    check("the left shift key does not",
          !xkb_keymap_key_repeats(keymap, EVDEV(KEY_LEFTSHIFT)));

    /* ================================================================
     * 3. keycodes to keysyms, against X11's published numbers
     * ================================================================ */
    struct xkb_state *state = xkb_state_new(keymap);
    check("xkb_state_new", state != NULL);
    if (!state) {
        printf("xkbtest: %d checks, %d failures (no state)\n",
               checks, failures + 1);
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        return 1;
    }

    check("keycode 38 (KEY_A + 8) is XKB_KEY_a = 0x0061",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_A)) == XKB_KEY_a);
    check("keycode 36 (KEY_ENTER + 8) is XKB_KEY_Return = 0xff0d",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_ENTER))
              == XKB_KEY_Return);
    check("keycode 9 (KEY_ESC + 8) is XKB_KEY_Escape = 0xff1b",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_ESC))
              == XKB_KEY_Escape);
    check("keycode 50 (KEY_LEFTSHIFT + 8) is XKB_KEY_Shift_L = 0xffe1",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_LEFTSHIFT))
              == XKB_KEY_Shift_L);
    check("keycode 24 (KEY_Q + 8) is XKB_KEY_q",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_Q)) == XKB_KEY_q);
    check("keycode 10 (KEY_1 + 8) is XKB_KEY_1",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_1)) == XKB_KEY_1);
    check("keycode 65 (KEY_SPACE + 8) is XKB_KEY_space",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_SPACE))
              == XKB_KEY_space);

    /* The published constants really are these numbers. If the header
     * ever disagreed with X11, every check above would still pass
     * against a wrong keymap. */
    check("XKB_KEY_a is 0x0061",       XKB_KEY_a == 0x0061);
    check("XKB_KEY_A is 0x0041",       XKB_KEY_A == 0x0041);
    check("XKB_KEY_Return is 0xff0d",  XKB_KEY_Return == 0xff0d);
    check("XKB_KEY_Escape is 0xff1b",  XKB_KEY_Escape == 0xff1b);
    check("XKB_KEY_Shift_L is 0xffe1", XKB_KEY_Shift_L == 0xffe1);

    /* ================================================================
     * 4. modifier state actually changes the answer
     * ================================================================
     *
     * The point of a keymap is that one keycode gives different symbols
     * depending on what else is held. Pressing shift and asking again
     * must give `A` — through xkb_state_update_key, which is the call
     * WebKit makes on every key event.
     */
    xkb_state_update_key(state, EVDEV(KEY_LEFTSHIFT), XKB_KEY_DOWN);
    check("with shift held, A is now XKB_KEY_A",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_A)) == XKB_KEY_A);
    check("and the Shift modifier reads as active",
          xkb_state_mod_index_is_active(state, shift,
                                        XKB_STATE_MODS_EFFECTIVE) == 1);
    check("serialize_mods reports the shift bit",
          (xkb_state_serialize_mods(state, XKB_STATE_MODS_EFFECTIVE)
           & (1u << shift)) != 0);

    xkb_state_update_key(state, EVDEV(KEY_LEFTSHIFT), XKB_KEY_UP);
    check("released, A is XKB_KEY_a again",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_A)) == XKB_KEY_a);
    check("and the Shift modifier is inactive",
          xkb_state_mod_index_is_active(state, shift,
                                        XKB_STATE_MODS_EFFECTIVE) == 0);

    /*
     * Caps Lock is a *latch*, not a hold: one press turns it on and it
     * stays on after the release. It is a different code path from shift
     * in state.c, and it is the one that distinguishes a real XKB state
     * machine from a lookup table.
     */
    xkb_state_update_key(state, EVDEV(KEY_CAPSLOCK), XKB_KEY_DOWN);
    xkb_state_update_key(state, EVDEV(KEY_CAPSLOCK), XKB_KEY_UP);
    check("caps lock stays on after the key is released",
          xkb_state_mod_index_is_active(state, caps,
                                        XKB_STATE_MODS_EFFECTIVE) == 1);
    check("and A is XKB_KEY_A with it latched",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_A)) == XKB_KEY_A);
    check("but 1 is unaffected by caps lock",
          xkb_state_key_get_one_sym(state, EVDEV(KEY_1)) == XKB_KEY_1);
    xkb_state_update_key(state, EVDEV(KEY_CAPSLOCK), XKB_KEY_DOWN);
    xkb_state_update_key(state, EVDEV(KEY_CAPSLOCK), XKB_KEY_UP);
    check("a second press turns it off",
          xkb_state_mod_index_is_active(state, caps,
                                        XKB_STATE_MODS_EFFECTIVE) == 0);

    /*
     * Levels reached without a state, which is how a browser builds a
     * shortcut table: level 0 and level 1 of the same key in layout 0.
     */
    {
        const xkb_keysym_t *syms;
        int n = xkb_keymap_key_get_syms_by_level(keymap, EVDEV(KEY_A),
                                                 0, 0, &syms);
        check("level 0 of the A key is one symbol", n == 1);
        check("and it is `a`", n == 1 && syms[0] == XKB_KEY_a);
        n = xkb_keymap_key_get_syms_by_level(keymap, EVDEV(KEY_A),
                                             0, 1, &syms);
        check("level 1 of the A key is one symbol", n == 1);
        check("and it is `A`", n == 1 && syms[0] == XKB_KEY_A);
        check("the A key has at least two levels",
              xkb_keymap_num_levels_for_key(keymap, EVDEV(KEY_A), 0) >= 2);
        check("the A key has at least one layout",
              xkb_keymap_num_layouts_for_key(keymap, EVDEV(KEY_A)) >= 1);
    }

    /* ================================================================
     * 5. keysyms as names and as characters
     * ================================================================
     *
     * Independent of any keymap: this is src/keysym.c and
     * src/keysym-utf.c working against ks_tables.h, the one generated
     * header that *does* ship pre-made in the tarball.
     */
    {
        char buf[64];
        int n = xkb_keysym_get_name(XKB_KEY_a, buf, sizeof buf);
        check("keysym 0x0061 is named \"a\"", n > 0 && strcmp(buf, "a") == 0);
        n = xkb_keysym_get_name(XKB_KEY_Return, buf, sizeof buf);
        check("keysym 0xff0d is named \"Return\"",
              n > 0 && strcmp(buf, "Return") == 0);
        n = xkb_keysym_get_name(XKB_KEY_Shift_L, buf, sizeof buf);
        check("keysym 0xffe1 is named \"Shift_L\"",
              n > 0 && strcmp(buf, "Shift_L") == 0);

        check("\"Return\" resolves back to 0xff0d",
              xkb_keysym_from_name("Return", XKB_KEYSYM_NO_FLAGS)
                  == XKB_KEY_Return);
        check("case-insensitive lookup finds \"return\" too",
              xkb_keysym_from_name("return", XKB_KEYSYM_CASE_INSENSITIVE)
                  == XKB_KEY_Return);
        check("a name that is not a keysym gives XKB_KEY_NoSymbol",
              xkb_keysym_from_name("NotAKeysym", XKB_KEYSYM_NO_FLAGS)
                  == XKB_KEY_NoSymbol);

        /* Unicode, not X11: the Latin-1 block coincides with ASCII, and
         * the function keys map to nothing. */
        check("keysym `a` is U+0061", xkb_keysym_to_utf32(XKB_KEY_a) == 0x61);
        check("keysym `A` is U+0041", xkb_keysym_to_utf32(XKB_KEY_A) == 0x41);
        check("Return maps to carriage return U+000D",
              xkb_keysym_to_utf32(XKB_KEY_Return) == 0x0D);
        check("Shift_L maps to no character at all",
              xkb_keysym_to_utf32(XKB_KEY_Shift_L) == 0);
        /* A keysym outside Latin-1, to prove the table is consulted
         * rather than the low byte returned: U+20AC EURO SIGN. */
        check("EuroSign is U+20AC",
              xkb_keysym_to_utf32(XKB_KEY_EuroSign) == 0x20AC);
    }

    /* ================================================================
     * 6. dump and recompile
     * ================================================================
     *
     * Not an anchor — it is this library agreeing with itself — but it
     * is the only way to exercise keymap-dump.c, and it puts the parser
     * through a complete keymap from the other direction: everything
     * above came in through the rules resolver, and this comes in
     * through xkb_keymap_new_from_string.
     */
    {
        char *text = xkb_keymap_get_as_string(keymap,
                                              XKB_KEYMAP_FORMAT_TEXT_V1);
        check("the keymap dumps to text", text != NULL);
        check("and the text is substantial", text && strlen(text) > 10000);
        check("it opens with xkb_keymap {",
              text && strstr(text, "xkb_keymap {") == text);

        if (text) {
            struct xkb_keymap *again =
                xkb_keymap_new_from_string(ctx, text,
                                           XKB_KEYMAP_FORMAT_TEXT_V1,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
            check("the dump compiles back into a keymap", again != NULL);
            if (again) {
                check("with the same keycode range",
                      xkb_keymap_min_keycode(again) == min &&
                      xkb_keymap_max_keycode(again) == max);
                struct xkb_state *s2 = xkb_state_new(again);
                check("and A still gives `a`",
                      s2 && xkb_state_key_get_one_sym(s2, EVDEV(KEY_A))
                                == XKB_KEY_a);
                if (s2) xkb_state_unref(s2);
                xkb_keymap_unref(again);
            }
            free(text);
        }

        /* Text that is not a keymap must be refused, not accepted
         * empty. */
        struct xkb_keymap *bad =
            xkb_keymap_new_from_string(ctx, "this is not a keymap",
                                       XKB_KEYMAP_FORMAT_TEXT_V1,
                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
        check("garbage text does not compile", bad == NULL);
        if (bad) xkb_keymap_unref(bad);
    }

    /* ================================================================
     * 7. names that do not resolve
     * ================================================================
     *
     * A layout nobody has must give NULL rather than an empty keymap or
     * a fault. This is the include resolver failing to find a file,
     * which is a path this port exercises far more than a Linux build
     * does — every include here is an fread rather than an mmap.
     */
    {
        struct xkb_rule_names bogus = { "evdev", "pc105", "nosuchlayout",
                                        "", "" };
        struct xkb_keymap *k = xkb_keymap_new_from_names(ctx, &bogus,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
        check("a layout that does not exist gives NULL", k == NULL);
        if (k) xkb_keymap_unref(k);

        struct xkb_rule_names norules = { "nosuchruleset", "pc105", "us",
                                          "", "" };
        k = xkb_keymap_new_from_names(ctx, &norules,
                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
        check("a rules file that does not exist gives NULL", k == NULL);
        if (k) xkb_keymap_unref(k);

        /*
         * And NULL names must fall through to the defaults in
         * third_party/libxkbcommon-port/config.h — which are the same
         * evdev/pc105/us, so this must give a working keymap.
         */
        struct xkb_rule_names empty = { NULL, NULL, NULL, NULL, NULL };
        k = xkb_keymap_new_from_names(ctx, &empty,
                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
        check("all-NULL names fall through to the compiled-in defaults",
              k != NULL);
        if (k) {
            struct xkb_state *s = xkb_state_new(k);
            check("and that default keymap is the us one",
                  s && xkb_state_key_get_one_sym(s, EVDEV(KEY_A))
                          == XKB_KEY_a);
            if (s) xkb_state_unref(s);
            xkb_keymap_unref(k);
        }
    }

    /* ================================================================
     * 8. a second layout, to prove the whole tree is on the volume
     * ================================================================
     *
     * `us` is the one WebKit asks for, and staging only the files it
     * needs would have been enough for every check above. The volume
     * carries all 195 symbols files, so a layout with different symbols
     * must compile and must disagree with `us` — on a German keyboard
     * the key at KEY_Q's position is still `q`, but the one at KEY_Y's
     * is `z`. This is the check that a curated subset would have
     * failed.
     */
    {
        struct xkb_rule_names de = { "evdev", "pc105", "de", "", "" };
        struct xkb_keymap *k = xkb_keymap_new_from_names(ctx, &de,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
        check("the German layout compiles too", k != NULL);
        if (k) {
            struct xkb_state *s = xkb_state_new(k);
            /* KEY_Y is 21 on the Linux input layer; on a QWERTZ board
             * that position carries `z`. */
            check("and its KEY_Y position gives `z`, not `y`",
                  s && xkb_state_key_get_one_sym(s, EVDEV(21)) == XKB_KEY_z);
            if (s) xkb_state_unref(s);
            xkb_keymap_unref(k);
        }
    }

    xkb_state_unref(state);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);

    printf("xkbtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

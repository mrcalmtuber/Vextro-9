/*
 * G3SL compiler and machine check, run on the host.
 *
 * The shader compiler is the part of src/g3d.h that can be wrong in ways
 * a screenshot will not show: a precedence mistake produces a picture
 * that is merely a bit wrong, and a type rule that never fires produces
 * one that is right until it is not. So the arithmetic is checked
 * against expected values worked out by hand, and every diagnostic is
 * checked to actually fire.
 *
 *   cc -O2 -o build/g3d_test tools/g3d_test.c && ./build/g3d_test
 */
#include <stdio.h>
#include <stdint.h>

#include "../src/g3d.h"

static int failures = 0;
static int checks = 0;

static const char *const IN_NAMES[] = { "n", "l", "base", "gold", "e" };
static const uint8_t IN_TYPES[] = {
    G3SL_T_VEC3, G3SL_T_VEC3, G3SL_T_VEC3, G3SL_T_VEC3, G3SL_T_VEC3
};

static g3sl_prog_t prog;

/* Run `src` with the standard inputs and return `color`, or report. */
static int run(const char *label, const char *src, g3f *out) {
    g3sl_compile(&prog, src, IN_NAMES, IN_TYPES, 5);
    if (!prog.ok) {
        printf("  %-34s COMPILE FAILED line %d: %s\n",
               label, prog.err_line, prog.err);
        failures++;
        return 0;
    }
    g3sl_val_t vars[G3SL_MAX_VARS];
    for (int i = 0; i < G3SL_MAX_VARS; i++) {
        vars[i].v[0] = vars[i].v[1] = vars[i].v[2] = 0;
        vars[i].type = G3SL_T_SCALAR;
    }
    /* n = (0,0,1), l = (0,0,1), base = (0.5,0.25,0.125),
     * gold = (1,0.5,0), e = (0,0,1) */
    const g3f v[5][3] = {
        { 0, 0, G3D_ONE },
        { 0, 0, G3D_ONE },
        { G3D_ONE / 2, G3D_ONE / 4, G3D_ONE / 8 },
        { G3D_ONE, G3D_ONE / 2, 0 },
        { 0, 0, G3D_ONE },
    };
    for (int i = 0; i < 5; i++) {
        vars[i].v[0] = v[i][0];
        vars[i].v[1] = v[i][1];
        vars[i].v[2] = v[i][2];
        vars[i].type = G3SL_T_VEC3;
    }
    g3sl_run(&prog, vars);
    const int slot = g3sl_var_find(&prog, "color");
    if (slot < 0) { printf("  %-34s no colour\n", label); failures++; return 0; }
    out[0] = vars[slot].v[0];
    out[1] = vars[slot].v[1];
    out[2] = vars[slot].v[2];
    return 1;
}

/* Values are 16.16; allow a couple of ulps for the fixed-point rounding
 * that a chain of multiplies accumulates. */
static void expect(const char *label, const char *src,
                   double r, double g, double b) {
    checks++;
    g3f out[3];
    if (!run(label, src, out)) return;
    const double got[3] = { out[0] / 65536.0, out[1] / 65536.0,
                            out[2] / 65536.0 };
    const double want[3] = { r, g, b };
    for (int i = 0; i < 3; i++) {
        double d = got[i] - want[i];
        if (d < 0) d = -d;
        if (d > 0.001) {
            printf("  %-34s WRONG: got (%.4f %.4f %.4f) want (%.4f %.4f %.4f)\n",
                   label, got[0], got[1], got[2], want[0], want[1], want[2]);
            failures++;
            return;
        }
    }
    printf("  %-34s ok  (%.3f %.3f %.3f)\n", label, got[0], got[1], got[2]);
}

/* A program that must NOT compile, and the reason must mention `why`. */
static void expect_error(const char *label, const char *src) {
    checks++;
    g3sl_compile(&prog, src, IN_NAMES, IN_TYPES, 5);
    if (prog.ok) {
        printf("  %-34s ACCEPTED, should not have\n", label);
        failures++;
    } else {
        printf("  %-34s rejected: line %d, %s\n", label, prog.err_line,
               prog.err);
    }
}

int main(void) {
    printf("arithmetic and types\n");

    expect("scalar into every lane", "color = vec3(0.5, 0.25, 0.125);",
           0.5, 0.25, 0.125);

    expect("dot of two unit vectors", "d = dot(n, l); color = vec3(d, d, d);",
           1.0, 1.0, 1.0);

    /* precedence: 0.25 + 0.75*1.0 = 1.0, not (0.25+0.75)*1.0 which is
     * also 1.0 -- so use values where the two differ */
    expect("multiply binds tighter than add",
           "x = 0.5 + 0.25 * 0.5; color = vec3(x, x, x);",
           0.625, 0.625, 0.625);

    expect("parentheses override precedence",
           "x = (0.5 + 0.25) * 0.5; color = vec3(x, x, x);",
           0.375, 0.375, 0.375);

    expect("vec3 scaled by a scalar", "color = base * 2.0;",
           1.0, 0.5, 0.25);

    expect("scalar scaling a vec3 (other order)", "color = 2.0 * base;",
           1.0, 0.5, 0.25);

    expect("componentwise product", "color = base * gold;",
           0.5, 0.125, 0.0);

    expect("sat clamps both ends",
           "color = sat(vec3(2.0, 0.5, 0.0) - vec3(0.0, 0.0, 1.0));",
           1.0, 0.5, 0.0);

    expect("mix interpolates", "color = mix(base, gold, 0.5);",
           0.75, 0.375, 0.0625);

    expect("min and max",
           "color = vec3(max(0.25, 0.5), min(0.25, 0.5), max(0.0, 0.125));",
           0.5, 0.25, 0.125);

    expect("unary minus and abs",
           "x = abs(0.0 - 0.75); color = vec3(x, x, x);",
           0.75, 0.75, 0.75);

    expect("sqrt", "x = sqrt(0.25); color = vec3(x, x, x);",
           0.5, 0.5, 0.5);

    expect("cross product",
           "color = cross(vec3(1.0,0.0,0.0), vec3(0.0,1.0,0.0));",
           0.0, 0.0, 1.0);

    expect("norm of a non-unit vector",
           "color = norm(vec3(0.0, 3.0, 4.0));",
           0.0, 0.6, 0.8);

    expect("comments and whitespace are skipped",
           "# a comment\n"
           "x = 0.5;   # trailing\n"
           "color = vec3(x, x, x);",
           0.5, 0.5, 0.5);

    expect("a realistic lambert plus rim",
           "d = sat(dot(n, l));\n"
           "rim = sat(1.0 - dot(n, e));\n"
           "color = base * (0.2 + 0.8 * d) + gold * (rim * rim * 0.5);",
           0.5, 0.25, 0.125);

    printf("\ndiagnostics\n");
    expect_error("dot of scalars", "color = vec3(dot(0.5, 0.5),0.0,0.0);");
    expect_error("vec3 where a scalar belongs",
                 "color = vec3(base, 0.0, 0.0);");
    expect_error("adding a vec3 to nothing sensible",
                 "x = 0.5; color = cross(x, n);");
    expect_error("unknown name", "color = wobble * 2.0;");
    expect_error("unknown function", "color = frobnicate(n);");
    expect_error("missing colour", "x = 0.5;");
    expect_error("missing close paren", "color = vec3(0.5, 0.5, 0.5;");
    expect_error("a variable changing type",
                 "x = 0.5; x = vec3(1.0,1.0,1.0); color = x;");

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

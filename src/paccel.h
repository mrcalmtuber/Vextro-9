#ifndef PACCEL_H
#define PACCEL_H

/*
 * Pointer acceleration.
 *
 * Move slowly and the pointer tracks the mouse exactly, one pixel per
 * count, so a single-pixel target can actually be hit. Move quickly and
 * the gain climbs exponentially until a flick throws the pointer across
 * the screen. The whole curve lives in one table below.
 *
 * Two things constrain how this is written.
 *
 * The kernel is compiled -mno-sse -mno-80387 on x86_64 and
 * -mgeneral-regs-only on aarch64, so there is no floating point here at
 * all: the gains are 16.16 fixed point, computed once and written down
 * rather than evaluated. exp() is not available and would not be worth
 * linking if it were.
 *
 * And this applies to *relative* input only -- a PS/2 or USB mouse, or
 * virtio-input's EV_REL. An absolute device (the VMware backdoor, a
 * virtio tablet) reports where the pointer *is*, not how far it moved,
 * and scaling that would make the guest pointer drift away from the
 * physical one until they no longer agree about which edge of the screen
 * you are on. Acceleration is a property of relative motion, so it is
 * applied where the motion is relative and nowhere else.
 */

/*
 * gain[speed], 16.16 fixed point, indexed by |dx| + |dy| in one report.
 *
 *   speed 0-3   1.00x   exact, one count to one pixel
 *   speed 8     1.78x
 *   speed 16    4.46x
 *   speed 24   11.19x
 *   speed 27+  14.00x   the ceiling
 *
 * Manhattan distance rather than Euclidean: no square root, and the
 * difference between the two is a constant factor the curve absorbs.
 */
#define PACCEL_STEPS 64
#define PACCEL_FLAT  3          /* at or below this, gain is exactly 1 */

static const uint32_t paccel_gain[PACCEL_STEPS] = {
      65536,   65536,   65536,   65536,   73523,   82484,   92536,  103814,
     116466,  130660,  146584,  164449,  184491,  206975,  232200,  260499,
     292247,  327864,  367822,  412650,  462941,  519361,  582658,  653668,
     733333,  822707,  917504,  917504,  917504,  917504,  917504,  917504,
     917504,  917504,  917504,  917504,  917504,  917504,  917504,  917504,
     917504,  917504,  917504,  917504,  917504,  917504,  917504,  917504,
     917504,  917504,  917504,  917504,  917504,  917504,  917504,  917504,
     917504,  917504,  917504,  917504,  917504,  917504,  917504,  917504,
};

/*
 * The fraction a scaled delta could not spend, carried into the next
 * report.
 *
 * Without this the pointer loses distance on every event: a gain of 2.5
 * applied to a delta of 1 truncates to 2, so a slow drag across the pad
 * arrives short, and consistently short in the same direction. Keeping
 * the remainder makes the error cancel instead of accumulate.
 */
static int32_t paccel_rem_x = 0, paccel_rem_y = 0;

/* How fast the pointer was last moved, for anything that wants to react
 * to it. Zero when still. */
static uint32_t paccel_speed = 0;

/*
 * Scale one relative report in place.
 *
 * Called with the raw counts from the device, and leaves the accelerated
 * ones in the same variables.
 */
static void paccel_apply(int32_t *dx, int32_t *dy) {
    int32_t rx = *dx, ry = *dy;

    uint32_t ax = (uint32_t)(rx < 0 ? -rx : rx);
    uint32_t ay = (uint32_t)(ry < 0 ? -ry : ry);
    uint32_t speed = ax + ay;
    paccel_speed = speed;

    if (speed == 0) return;

    /* Below the knee the pointer is exactly 1:1, and the accumulators are
     * cleared so a slow, deliberate movement cannot inherit a fraction
     * left over from a previous flick. */
    if (speed <= PACCEL_FLAT) {
        paccel_rem_x = paccel_rem_y = 0;
        return;
    }

    uint32_t g = paccel_gain[speed < PACCEL_STEPS ? speed : PACCEL_STEPS - 1];

    /* 16.16 multiply, keeping the remainder rather than discarding it. */
    int32_t nx = rx * (int32_t)(g >> 8);      /* 8.8 to stay well inside 32 bits */
    int32_t ny = ry * (int32_t)(g >> 8);
    nx += paccel_rem_x;
    ny += paccel_rem_y;

    *dx = nx >> 8;
    *dy = ny >> 8;
    paccel_rem_x = nx - (*dx << 8);
    paccel_rem_y = ny - (*dy << 8);
}

/* A flick is worth showing. The threshold is above the knee, so ordinary
 * movement never triggers it. */
#define PACCEL_FLING 10

static int paccel_is_fling(void) { return paccel_speed >= PACCEL_FLING; }

#endif /* PACCEL_H */

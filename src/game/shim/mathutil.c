/*
 * mathutil.c - PC port implementation of SMB2's DOL-side "mathutil" engine.
 *
 * The original is hand-written PPC asm (Sega) living in main.dol, with the
 * working matrix (mtxA), a second matrix (mtxB) and a matrix stack kept in
 * the GameCube's locked cache.  This is a portable plain-C reimplementation
 * with the same semantics, written against the SMB2 decomp's declarations
 * (decomp/include/mathutil.h) using SMB1's C_ONLY mathutil implementation
 * as the semantic reference.
 *
 * Angles are s16 with 65536 units per full circle.  The s16 trig
 * (math_sin, math_sin_cos_v, math_tan, math_atan2, math_atan and the
 * mtxa_rotate_* helpers) is a bit-exact port of the DOL's table-driven asm,
 * using the DOL's own tables (mathutil_tables_gen.c), so it matches the
 * GameCube and needs no libm.  Other libm calls here go through
 * pc_detmath.h (docs/detmath.md).
 */

#include <math.h>
#include <float.h>
#include <string.h>

#include "mathutil.h"

/* ------------------------------------------------------------------ */
/* State: mtxA/mtxB backing storage and the matrix stack.             */
/* ------------------------------------------------------------------ */

/* Defined below; not declared in the decomp's mathutil.h (REL code declares
 * these locally), so give prototypes for what we use early. */
void mtx_from_identity(Mtx mtx);

#define MTX_STACK_DEPTH 32

static struct MathutilData mathutil_data;
static Mtx mtx_stack[MTX_STACK_DEPTH];
static Mtx *mtx_stack_ptr = &mtx_stack[MTX_STACK_DEPTH];  /* grows downward */

/* The decomp's REL code accesses the current matrix as mtxa->mtxA. */
struct MathutilData *mtxa = &mathutil_data;

#define MTXA (mathutil_data.mtxA)
#define MTXB (mathutil_data.mtxB)

/* Statically initialized above; the constructor just makes doubly sure the
 * pointers are sane and the matrices hold identity before game code runs. */
__attribute__((constructor))
static void mathutil_shim_init(void)
{
    mtxa = &mathutil_data;
    mtx_stack_ptr = &mtx_stack[MTX_STACK_DEPTH];
    mtx_from_identity(MTXA);
    mtx_from_identity(MTXB);
}

/* ------------------------------------------------------------------ */
/* Angle helpers: s16 angles, 65536 = full circle.                    */
/* ------------------------------------------------------------------ */

#define MU_PI 3.14159265358979323846f

/* DOL lookup tables (float bit patterns), docs/detmath.md:
 * sin/tan: 0x4001 entries over a quarter turn; atan: 0x1000 (slope,
 * intercept) pairs over r in [0,1) giving the angle in s16 units. */
extern const u32 g_mathutil_sin_table[0x4001];
extern const u32 g_mathutil_tan_table[0x4001];
extern const u32 g_mathutil_atan_table[0x2000];

static float tbl(const u32 *t, int i)
{
    float f;
    memcpy(&f, &t[i], sizeof f);
    return f;
}

/* asm 0x80061E94 (g_math_sin_cos_wrapper): one table read per value; bit
 * 0x4000 mirrors the quarter, 0x8000 negates sin, bit14^bit15 negates cos.
 * The asm negates with fnabs, so sin(0x8000) and cos(0x4000) are -0.0. */
static void sin_cos(s16 angle, float *s, float *c)
{
    int a = (u16)angle;
    int i = a & 0x3FFF;

    if (a & 0x4000)
        i = 0x4000 - i;
    *s = tbl(g_mathutil_sin_table, i);
    *c = tbl(g_mathutil_sin_table, 0x4000 - i);
    if (a & 0x8000)
        *s = -fabsf(*s);
    if (((a >> 15) ^ (a >> 14)) & 1)
        *c = -fabsf(*c);
}

/* PPC fmadds: a * b + c with a single rounding to float.  The product of
 * two floats is exact in double; TwoSum gives the exact error of the
 * double sum, and rounding that sum to odd before the final double->float
 * rounding makes the result correctly rounded (53 >= 24 + 2 bits). */
static float fmadds_exact(float a, float b, float c)
{
    double p = (double)a * (double)b;
    double s = p + (double)c;
    double bb = s - p;
    double e = (p - (s - bb)) + ((double)c - bb);

    if (e != 0.0)
    {
        u64 u;

        memcpy(&u, &s, sizeof u);
        if ((u & 1) == 0)
        {
            if ((e > 0.0) == (s > 0.0))
                u++;
            else
                u--;
            memcpy(&s, &u, sizeof s);
        }
    }
    return (float)s;
}

/* asm 0x80061FC8: angle (s16 units) of atan(r), r = min/max in [0,1]:
 * index = floor(r * 4096) taken from the float's bits (srw of the
 * mantissa, mask 0x7FF8: r rounded up to 1.0 wraps to entry 0, as on the
 * GC), then fctiw(fmadds(r, slope, intercept)). */
static int atan_octant(float r)
{
    u32 bits;
    u32 m, idx;
    int sh;

    memcpy(&bits, &r, sizeof bits);
    m = (bits & 0x7FFFFF) | 0x800000;
    sh = 0x87 - (int)((bits >> 23) & 0xFF);
    idx = (sh & ~31) ? 0 : (m >> sh);
    idx = (idx & 0x7FF8) >> 2;  /* byte offset / 4 = float index */
    return (int)pc_lrintf(fmadds_exact(r, tbl(g_mathutil_atan_table, idx),
                                       tbl(g_mathutil_atan_table, idx + 1)));
}

/* asm 0x8006208C: |x| == |y| (or |x| == 1 for atan): +-0x2000 / +-0x6000 */
static s16 atan_diagonal(int yneg, int signsDiffer)
{
    return (s16)((yneg ? 0x8000 : 0) | (signsDiffer ? 0x4000 : 0) | 0x2000);
}

/* asm 0x80062014..0x8006209C: fold the octant angle back */
static s16 atan_fold(int n, int swapped, int signsDiffer, int yneg)
{
    if (swapped)
        n = 0x4000 - n;
    if (signsDiffer)
        n = 0x8000 - n;
    if (yneg)
        n -= 0x8000;
    return (s16)n;
}

static int is_inf_or_nan(float x) { return !(fabsf(x) <= FLT_MAX); }

/* ------------------------------------------------------------------ */
/* Scalar math                                                        */
/* ------------------------------------------------------------------ */

float math_sqrt(float x)
{
    /* Original returns 0 for x <= 0 (and for NaN inputs). */
    if (!(x > 0.0f))
        return 0.0f;
    return sqrtf(x);
}

float math_rsqrt(float x)
{
    /* Original returns +inf for x <= 0. */
    if (!(x > 0.0f))
        return INFINITY;
    return 1.0f / sqrtf(x);
}

/* asm 0x80061E60 */
float math_sin(s16 angle)
{
    int a = (u16)angle;
    int i = a & 0x3FFF;
    float f;

    if (a & 0x4000)
        i = 0x4000 - i;
    f = tbl(g_mathutil_sin_table, i);
    return (a & 0x8000) ? -f : f;
}

void math_sin_cos_v(s16 angle, float *sincos)
{
    sin_cos(angle, &sincos[0], &sincos[1]);
}

/* asm 0x80061EF4: period 0x8000; the upper half of the quarter reads the
 * table backwards and negates (tan(0x4000) = -inf from the last entry). */
float math_tan(s16 angle)
{
    int a = (u16)angle;

    if (a & 0x4000)
        return -fabsf(tbl(g_mathutil_tan_table, 0x4000 - (a & 0x3FFF)));
    return tbl(g_mathutil_tan_table, a & 0x3FFF);
}

/* asm 0x80061F24, including its special cases: 0 for (0,0) and any NaN;
 * y infinite: 0x4000 | (signs differ ? 0x8000 : 0); x infinite, y finite:
 * signs differ ? 0x8000 : 0; both infinite: the diagonals. */
s16 math_atan2(float y, float x)
{
    int xneg = x < 0.0f, yneg = y < 0.0f;
    int differ = xneg ^ yneg;
    float ax, ay;

    if (is_inf_or_nan(x) || is_inf_or_nan(y))
    {
        if (x != x || y != y)
            return 0;
        if (!is_inf_or_nan(x))
            return (s16)(0x4000 | (differ ? 0x8000 : 0));
        if (!is_inf_or_nan(y))
            return (s16)(differ ? 0x8000 : 0);
        return atan_diagonal(yneg, differ);
    }
    if (x == 0.0f && y == 0.0f)
        return 0;
    ax = fabsf(x);
    ay = fabsf(y);
    if (ax == ay)
        return atan_diagonal(yneg, differ);
    if (ax > ay)
        return atan_fold(atan_octant(ay / ax), 0, differ, yneg);
    return atan_fold(atan_octant(ax / ay), 1, differ, yneg);
}

/* asm 0x80061F84: same core as math_atan2 with x as y/1.  (For +-inf the
 * asm's result depends on the caller's CR7; this takes the x > 0 case,
 * which gives the mathematically right +-0x4000.) */
s16 math_atan(float x)
{
    int neg = x < 0.0f;
    float ax;

    if (is_inf_or_nan(x))
    {
        if (x != x)
            return 0;
        return (s16)(0x4000 | (neg ? 0x8000 : 0));
    }
    if (x == 0.0f)
        return 0;
    ax = fabsf(x);
    if (ax == 1.0f)
        return atan_diagonal(neg, neg);
    if (ax < 1.0f)
        return atan_fold(atan_octant(ax), 0, neg, neg);
    return atan_fold(atan_octant(1.0f / ax), 1, neg, neg);
}

/* ------------------------------------------------------------------ */
/* Vector functions                                                   */
/* ------------------------------------------------------------------ */

static float vec_sq_len(const Vec *v)
{
    return v->x * v->x + v->y * v->y + v->z * v->z;
}

static void vec_cross(const Vec *a, const Vec *b, Vec *out)
{
    float x = a->y * b->z - a->z * b->y;
    float y = a->z * b->x - a->x * b->z;
    float z = a->x * b->y - a->y * b->x;

    out->x = x;
    out->y = y;
    out->z = z;
}

float vec_dot_normalized(Vec *a, Vec *b)
{
    float sqA = vec_sq_len(a);
    float sqB = vec_sq_len(b);
    float dot = a->x * b->x + a->y * b->y + a->z * b->z;

    return math_rsqrt(sqA * sqB) * dot;
}

float vec_dot_normalized_safe(Vec *a, Vec *b)
{
    float dot = a->x * b->x + a->y * b->y + a->z * b->z;
    float prod = vec_sq_len(a) * vec_sq_len(b);

    if (prod > FLT_EPSILON)
        return math_rsqrt(prod) * dot;
    return 0.0f;
}

float vec_normalize_len(Vec *v)
{
    float sqLen = vec_sq_len(v);

    if (sqLen > FLT_EPSILON)
    {
        float lenRecip = 1.0f / sqrtf(sqLen);

        v->x *= lenRecip;
        v->y *= lenRecip;
        v->z *= lenRecip;
        return lenRecip * sqLen;  /* (1/len) * len^2 = len */
    }
    v->x = v->y = v->z = 0.0f;
    return 0.0f;
}

void vec_set_len(float len, Vec *src, Vec *dst)
{
    float sqLen = vec_sq_len(src);

    if (sqLen > FLT_EPSILON)
    {
        float scale = (1.0f / sqrtf(sqLen)) * len;

        dst->x = src->x * scale;
        dst->y = src->y * scale;
        dst->z = src->z * scale;
    }
    else
        dst->x = dst->y = dst->z = 0.0f;
}

void vec_to_euler(Vec *v, S16Vec *rot)
{
    float negX = -v->x;
    float y    =  v->y;
    float negZ = -v->z;

    rot->x = math_atan2(y, math_sqrt(negX * negX + negZ * negZ));
    rot->y = math_atan2(negX, negZ);
    rot->z = 0;
}

void vec_to_euler_xy(Vec *v, s16 *rotX, s16 *rotY)
{
    float negX = -v->x;
    float y    =  v->y;
    float negZ = -v->z;

    *rotX = math_atan2(y, math_sqrt(negX * negX + negZ * negZ));
    *rotY = math_atan2(negX, negZ);
}

/* ------------------------------------------------------------------ */
/* Ray functions                                                      */
/* ------------------------------------------------------------------ */

void ray_scale(Vec *rayStart, Vec *rayEnd, Vec *outRayEnd, float scale)
{
    outRayEnd->x = (rayEnd->x - rayStart->x) * scale + rayStart->x;
    outRayEnd->y = (rayEnd->y - rayStart->y) * scale + rayStart->y;
    outRayEnd->z = (rayEnd->z - rayStart->z) * scale + rayStart->z;
}

void ray_to_euler(Vec *rayStart, Vec *rayEnd, S16Vec *rot)
{
    float negX = rayStart->x - rayEnd->x;
    float y    = rayEnd->y - rayStart->y;
    float negZ = rayStart->z - rayEnd->z;

    rot->x = math_atan2(y, math_sqrt(negX * negX + negZ * negZ));
    rot->y = math_atan2(negX, negZ);
    rot->z = 0;
}

void ray_to_euler_xy(Vec *rayStart, Vec *rayEnd, s16 *rotX, s16 *rotY)
{
    float negX = rayStart->x - rayEnd->x;
    float y    = rayEnd->y - rayStart->y;
    float negZ = rayStart->z - rayEnd->z;

    *rotX = math_atan2(y, math_sqrt(negX * negX + negZ * negZ));
    *rotY = math_atan2(negX, negZ);
}

/* ------------------------------------------------------------------ */
/* Plain matrix functions                                             */
/* ------------------------------------------------------------------ */

void mtx_copy(Mtx src, Mtx dst)
{
    memcpy(dst, src, sizeof(Mtx));
}

void mtx_from_identity(Mtx mtx)
{
    int row, col;

    for (row = 0; row < 3; row++)
        for (col = 0; col < 4; col++)
            mtx[row][col] = (row == col) ? 1.0f : 0.0f;
}

/* result = a * b (row-major affine concatenation; a's translation applies
 * last).  Safe when result aliases a and/or b. */
static void mtx_mult(const Mtx a, const Mtx b, Mtx result)
{
    Mtx tmp;
    int row, col;

    for (row = 0; row < 3; row++)
    {
        for (col = 0; col < 4; col++)
        {
            tmp[row][col] = a[row][0] * b[0][col]
                          + a[row][1] * b[1][col]
                          + a[row][2] * b[2][col];
        }
        tmp[row][3] += a[row][3];
    }
    memcpy(result, tmp, sizeof(Mtx));
}

/* ------------------------------------------------------------------ */
/* mtxA stack                                                         */
/* ------------------------------------------------------------------ */

void mtxa_push(void)
{
    if (mtx_stack_ptr > &mtx_stack[0])
        mtx_stack_ptr--;
    mtx_copy(MTXA, *mtx_stack_ptr);
}

void mtxa_pop(void)
{
    mtx_copy(*mtx_stack_ptr, MTXA);
    if (mtx_stack_ptr < &mtx_stack[MTX_STACK_DEPTH])
        mtx_stack_ptr++;
}

void mtxa_peek(void)
{
    mtx_copy(*mtx_stack_ptr, MTXA);
}

/* ------------------------------------------------------------------ */
/* mtxA loads/stores                                                  */
/* ------------------------------------------------------------------ */

void mtxa_from_identity(void)
{
    mtx_from_identity(MTXA);
}

void mtxa_sq_from_identity(void)
{
    MTXA[0][0] = 1.0f; MTXA[0][1] = 0.0f; MTXA[0][2] = 0.0f;
    MTXA[1][0] = 0.0f; MTXA[1][1] = 1.0f; MTXA[1][2] = 0.0f;
    MTXA[2][0] = 0.0f; MTXA[2][1] = 0.0f; MTXA[2][2] = 1.0f;
}

void mtxa_from_mtx(Mtx m)
{
    mtx_copy(m, MTXA);
}

void mtxa_to_mtx(Mtx m)
{
    mtx_copy(MTXA, m);
}

void mtxa_sq_from_mtx(Mtx m)
{
    MTXA[0][0] = m[0][0]; MTXA[0][1] = m[0][1]; MTXA[0][2] = m[0][2];
    MTXA[1][0] = m[1][0]; MTXA[1][1] = m[1][1]; MTXA[1][2] = m[1][2];
    MTXA[2][0] = m[2][0]; MTXA[2][1] = m[2][1]; MTXA[2][2] = m[2][2];
}

void mtxa_sq_to_mtx(Mtx m)
{
    m[0][0] = MTXA[0][0]; m[0][1] = MTXA[0][1]; m[0][2] = MTXA[0][2];
    m[1][0] = MTXA[1][0]; m[1][1] = MTXA[1][1]; m[1][2] = MTXA[1][2];
    m[2][0] = MTXA[2][0]; m[2][1] = MTXA[2][1]; m[2][2] = MTXA[2][2];
}

void mtxa_to_mtxb(void)
{
    mtx_copy(MTXA, MTXB);
}

void mtxa_from_mtxb(void)
{
    mtx_copy(MTXB, MTXA);
}

void mtxa_from_mtxb_mult_mtx(Mtx m)
{
    mtx_mult(MTXB, m, MTXA);
}

/* ------------------------------------------------------------------ */
/* mtxA constructors                                                  */
/* ------------------------------------------------------------------ */

void mtxa_from_translate_xyz(float x, float y, float z)
{
    MTXA[0][0] = 1.0f; MTXA[0][1] = 0.0f; MTXA[0][2] = 0.0f; MTXA[0][3] = x;
    MTXA[1][0] = 0.0f; MTXA[1][1] = 1.0f; MTXA[1][2] = 0.0f; MTXA[1][3] = y;
    MTXA[2][0] = 0.0f; MTXA[2][1] = 0.0f; MTXA[2][2] = 1.0f; MTXA[2][3] = z;
}

void mtxa_from_translate(Vec *v)
{
    mtxa_from_translate_xyz(v->x, v->y, v->z);
}

void mtxa_from_rotate_x(s16 angle)
{
    float s, c;

    sin_cos(angle, &s, &c);
    MTXA[0][0] = 1.0f; MTXA[0][1] = 0.0f; MTXA[0][2] = 0.0f; MTXA[0][3] = 0.0f;
    MTXA[1][0] = 0.0f; MTXA[1][1] = c;    MTXA[1][2] = -s;   MTXA[1][3] = 0.0f;
    MTXA[2][0] = 0.0f; MTXA[2][1] = s;    MTXA[2][2] = c;    MTXA[2][3] = 0.0f;
}

void mtxa_from_rotate_y(s16 angle)
{
    float s, c;

    sin_cos(angle, &s, &c);
    MTXA[0][0] = c;    MTXA[0][1] = 0.0f; MTXA[0][2] = s;    MTXA[0][3] = 0.0f;
    MTXA[1][0] = 0.0f; MTXA[1][1] = 1.0f; MTXA[1][2] = 0.0f; MTXA[1][3] = 0.0f;
    MTXA[2][0] = -s;   MTXA[2][1] = 0.0f; MTXA[2][2] = c;    MTXA[2][3] = 0.0f;
}

void mtxa_from_rotate_z(s16 angle)
{
    float s, c;

    sin_cos(angle, &s, &c);
    MTXA[0][0] = c;    MTXA[0][1] = -s;   MTXA[0][2] = 0.0f; MTXA[0][3] = 0.0f;
    MTXA[1][0] = s;    MTXA[1][1] = c;    MTXA[1][2] = 0.0f; MTXA[1][3] = 0.0f;
    MTXA[2][0] = 0.0f; MTXA[2][1] = 0.0f; MTXA[2][2] = 1.0f; MTXA[2][3] = 0.0f;
}

void mtxa_from_mtxb_translate_xyz(float x, float y, float z)
{
    int row;

    for (row = 0; row < 3; row++)
    {
        float t = MTXB[row][0] * x + MTXB[row][1] * y + MTXB[row][2] * z
                + MTXB[row][3];

        MTXA[row][0] = MTXB[row][0];
        MTXA[row][1] = MTXB[row][1];
        MTXA[row][2] = MTXB[row][2];
        MTXA[row][3] = t;
    }
}

void mtxa_from_mtxb_translate(Vec *v)
{
    mtxa_from_mtxb_translate_xyz(v->x, v->y, v->z);
}

void mtxa_from_quat(Quat *q)
{
    MTXA[0][3] = MTXA[1][3] = MTXA[2][3] = 0.0f;

    MTXA[0][0] = 1.0f - (q->z * q->z + q->y * q->y) * 2.0f;
    MTXA[1][1] = 1.0f - (q->x * q->x + q->z * q->z) * 2.0f;
    MTXA[2][2] = 1.0f - (q->y * q->y + q->x * q->x) * 2.0f;

    MTXA[1][2] = q->y * (q->z + q->z) - q->w * (q->x + q->x);
    MTXA[2][1] = q->y * (q->z + q->z) + q->w * (q->x + q->x);
    MTXA[2][0] = q->x * (q->z + q->z) - q->w * (q->y + q->y);
    MTXA[0][2] = q->x * (q->z + q->z) + q->w * (q->y + q->y);
    MTXA[0][1] = q->x * (q->y + q->y) - q->w * (q->z + q->z);
    MTXA[1][0] = q->x * (q->y + q->y) + q->w * (q->z + q->z);
}

/* ------------------------------------------------------------------ */
/* mtxA concatenation / modification                                  */
/* ------------------------------------------------------------------ */

void mtxa_mult_right(Mtx m)
{
    mtx_mult(MTXA, m, MTXA);
}

void mtxa_mult_left(Mtx m)
{
    mtx_mult(m, MTXA, MTXA);
}

void mtxa_translate_xyz(float x, float y, float z)
{
    float a = MTXA[0][0] * x + MTXA[0][1] * y + MTXA[0][2] * z + MTXA[0][3];
    float b = MTXA[1][0] * x + MTXA[1][1] * y + MTXA[1][2] * z + MTXA[1][3];
    float c = MTXA[2][0] * x + MTXA[2][1] * y + MTXA[2][2] * z + MTXA[2][3];

    MTXA[0][3] = a;
    MTXA[1][3] = b;
    MTXA[2][3] = c;
}

void mtxa_translate(Vec *v)
{
    mtxa_translate_xyz(v->x, v->y, v->z);
}

void mtxa_translate_neg_xyz(float x, float y, float z)
{
    mtxa_translate_xyz(-x, -y, -z);
}

void mtxa_translate_neg(Vec *v)
{
    mtxa_translate_xyz(-v->x, -v->y, -v->z);
}

void mtxa_scale_xyz(float x, float y, float z)
{
    int row;

    for (row = 0; row < 3; row++)
    {
        MTXA[row][0] *= x;
        MTXA[row][1] *= y;
        MTXA[row][2] *= z;
    }
}

void mtxa_scale(Vec *s)
{
    mtxa_scale_xyz(s->x, s->y, s->z);
}

void mtxa_scale_s(float s)
{
    mtxa_scale_xyz(s, s, s);
}

void mtxa_rotate_x_sin_cos(float s, float c)
{
    int row;

    for (row = 0; row < 3; row++)
    {
        float m1 = MTXA[row][1];
        float m2 = MTXA[row][2];

        MTXA[row][1] = m1 * c + m2 * s;
        MTXA[row][2] = m1 * -s + m2 * c;
    }
}

void mtxa_rotate_y_sin_cos(float s, float c)
{
    int row;

    for (row = 0; row < 3; row++)
    {
        float m0 = MTXA[row][0];
        float m2 = MTXA[row][2];

        MTXA[row][2] = m0 * s + m2 * c;
        MTXA[row][0] = m0 * c + m2 * -s;
    }
}

void mtxa_rotate_z_sin_cos(float s, float c)
{
    int row;

    for (row = 0; row < 3; row++)
    {
        float m0 = MTXA[row][0];
        float m1 = MTXA[row][1];

        MTXA[row][0] = m0 * c + m1 * s;
        MTXA[row][1] = m0 * -s + m1 * c;
    }
}

void mtxa_rotate_x(s16 angle)
{
    float s, c;

    sin_cos(angle, &s, &c);
    mtxa_rotate_x_sin_cos(s, c);
}

void mtxa_rotate_y(s16 angle)
{
    float s, c;

    sin_cos(angle, &s, &c);
    mtxa_rotate_y_sin_cos(s, c);
}

void mtxa_rotate_z(s16 angle)
{
    float s, c;

    sin_cos(angle, &s, &c);
    mtxa_rotate_z_sin_cos(s, c);
}

void mtxa_normalize_basis(void)
{
    float iRecip = math_rsqrt(MTXA[0][0] * MTXA[0][0] + MTXA[1][0] * MTXA[1][0] + MTXA[2][0] * MTXA[2][0]);
    float jRecip = math_rsqrt(MTXA[0][1] * MTXA[0][1] + MTXA[1][1] * MTXA[1][1] + MTXA[2][1] * MTXA[2][1]);
    float kRecip = math_rsqrt(MTXA[0][2] * MTXA[0][2] + MTXA[1][2] * MTXA[1][2] + MTXA[2][2] * MTXA[2][2]);

    MTXA[0][0] *= iRecip;
    MTXA[1][0] *= iRecip;
    MTXA[2][0] *= iRecip;

    MTXA[0][1] *= jRecip;
    MTXA[1][1] *= jRecip;
    MTXA[2][1] *= jRecip;

    MTXA[0][2] *= kRecip;
    MTXA[1][2] *= kRecip;
    MTXA[2][2] *= kRecip;
}

/* ------------------------------------------------------------------ */
/* mtxA inversion                                                     */
/* ------------------------------------------------------------------ */

void mtxa_invert(void)
{
    float m00 = MTXA[0][0], m01 = MTXA[0][1], m02 = MTXA[0][2], t0 = MTXA[0][3];
    float m10 = MTXA[1][0], m11 = MTXA[1][1], m12 = MTXA[1][2], t1 = MTXA[1][3];
    float m20 = MTXA[2][0], m21 = MTXA[2][1], m22 = MTXA[2][2], t2 = MTXA[2][3];

    /* Cofactors of the 3x3 part (adjugate layout). */
    float c00 = m11 * m22 - m21 * m12;
    float c01 = m21 * m02 - m01 * m22;
    float c02 = m01 * m12 - m11 * m02;
    float c10 = m20 * m12 - m10 * m22;
    float c11 = m00 * m22 - m20 * m02;
    float c12 = m10 * m02 - m00 * m12;
    float c20 = m10 * m21 - m20 * m11;
    float c21 = m20 * m01 - m00 * m21;
    float c22 = m00 * m11 - m10 * m01;

    float det = m00 * c00 + m10 * c01 + m20 * c02;
    float rdet = 1.0f / det;  /* no zero check, like the original */

    MTXA[0][0] = c00 * rdet;
    MTXA[0][1] = c01 * rdet;
    MTXA[0][2] = c02 * rdet;
    MTXA[0][3] = -(c00 * t0 + c01 * t1 + c02 * t2) * rdet;

    MTXA[1][0] = c10 * rdet;
    MTXA[1][1] = c11 * rdet;
    MTXA[1][2] = c12 * rdet;
    MTXA[1][3] = -(c10 * t0 + c11 * t1 + c12 * t2) * rdet;

    MTXA[2][0] = c20 * rdet;
    MTXA[2][1] = c21 * rdet;
    MTXA[2][2] = c22 * rdet;
    MTXA[2][3] = -(c20 * t0 + c21 * t1 + c22 * t2) * rdet;
}

void mtxa_rigid_invert(void)
{
    float m00 = MTXA[0][0], m01 = MTXA[0][1], m02 = MTXA[0][2], t0 = MTXA[0][3];
    float m10 = MTXA[1][0], m11 = MTXA[1][1], m12 = MTXA[1][2], t1 = MTXA[1][3];
    float m20 = MTXA[2][0], m21 = MTXA[2][1], m22 = MTXA[2][2], t2 = MTXA[2][3];

    /* Transpose the rotation part. */
    MTXA[0][0] = m00; MTXA[0][1] = m10; MTXA[0][2] = m20;
    MTXA[1][0] = m01; MTXA[1][1] = m11; MTXA[1][2] = m21;
    MTXA[2][0] = m02; MTXA[2][1] = m12; MTXA[2][2] = m22;

    /* New translation = -(R^T * t). */
    MTXA[0][3] = -(m00 * t0 + m10 * t1 + m20 * t2);
    MTXA[1][3] = -(m01 * t0 + m11 * t1 + m21 * t2);
    MTXA[2][3] = -(m02 * t0 + m12 * t1 + m22 * t2);
}

/* ------------------------------------------------------------------ */
/* mtxA transforms                                                    */
/* ------------------------------------------------------------------ */

static void mtxa_tf(Vec *dst, float x, float y, float z, float w)
{
    float tx = MTXA[0][0] * x + MTXA[0][1] * y + MTXA[0][2] * z + MTXA[0][3] * w;
    float ty = MTXA[1][0] * x + MTXA[1][1] * y + MTXA[1][2] * z + MTXA[1][3] * w;
    float tz = MTXA[2][0] * x + MTXA[2][1] * y + MTXA[2][2] * z + MTXA[2][3] * w;

    dst->x = tx;
    dst->y = ty;
    dst->z = tz;
}

void mtxa_tf_point(Vec *src, Vec *dst)
{
    mtxa_tf(dst, src->x, src->y, src->z, 1.0f);
}

void mtxa_tf_vec(Vec *src, Vec *dst)
{
    mtxa_tf(dst, src->x, src->y, src->z, 0.0f);
}

void mtxa_tf_point_xyz(float x, float y, float z, Vec *dst)
{
    mtxa_tf(dst, x, y, z, 1.0f);
}

void mtxa_tf_vec_xyz(float x, float y, float z, Vec *dst)
{
    mtxa_tf(dst, x, y, z, 0.0f);
}

/* Multiply (x, y, z) by the transpose of mtxA's 3x3 part (the inverse for a
 * rigid matrix), optionally negating the result. */
static void mtxa_rigid_inv_tf(Vec *dst, float x, float y, float z, BOOL negate)
{
    float tx = MTXA[0][0] * x + MTXA[1][0] * y + MTXA[2][0] * z;
    float ty = MTXA[0][1] * x + MTXA[1][1] * y + MTXA[2][1] * z;
    float tz = MTXA[0][2] * x + MTXA[1][2] * y + MTXA[2][2] * z;

    if (negate)
    {
        tx = -tx;
        ty = -ty;
        tz = -tz;
    }
    dst->x = tx;
    dst->y = ty;
    dst->z = tz;
}

void mtxa_rigid_inv_tf_point(Vec *src, Vec *dst)
{
    mtxa_rigid_inv_tf(dst, src->x - MTXA[0][3], src->y - MTXA[1][3],
                      src->z - MTXA[2][3], FALSE);
}

void mtxa_rigid_inv_tf_point_xyz(float x, float y, float z, Vec *dst)
{
    mtxa_rigid_inv_tf(dst, x - MTXA[0][3], y - MTXA[1][3], z - MTXA[2][3],
                      FALSE);
}

void mtxa_rigid_inv_tf_tl(Vec *dst)
{
    mtxa_rigid_inv_tf(dst, MTXA[0][3], MTXA[1][3], MTXA[2][3], TRUE);
}

void mtxa_rigid_inv_tf_vec(Vec *src, Vec *dst)
{
    mtxa_rigid_inv_tf(dst, src->x, src->y, src->z, FALSE);
}

void mtxa_rigid_inv_tf_vec_xyz(float x, float y, float z, Vec *dst)
{
    mtxa_rigid_inv_tf(dst, x, y, z, FALSE);
}

/* ------------------------------------------------------------------ */
/* mtxA <-> euler / quaternion                                        */
/* ------------------------------------------------------------------ */

void mtxa_to_euler_yxz(s16 *rotY, s16 *rotX, s16 *rotZ)
{
    Vec forward, up;

    mtxa_push();
    forward.x = 0.0f;
    forward.y = 0.0f;
    forward.z = -1.0f;
    up.x = 0.0f;
    up.y = 1.0f;
    up.z = 0.0f;
    mtxa_tf_vec(&forward, &forward);
    mtxa_tf_vec(&up, &up);
    *rotX = math_atan2(forward.y, math_sqrt(forward.x * forward.x + forward.z * forward.z));
    *rotY = math_atan2(forward.x, forward.z) - 0x8000;
    mtxa_from_rotate_y(*rotY);
    mtxa_rotate_x(*rotX);
    mtxa_rigid_inv_tf_vec(&up, &up);
    *rotZ = -math_atan2(up.x, up.y);
    mtxa_pop();
}

void mtxa_to_euler(S16Vec *rot)
{
    mtxa_to_euler_yxz(&rot->y, &rot->x, &rot->z);
}

void mtxa_to_quat(Quat *q)
{
    static const int next[] = {1, 2, 0};
    float trace = MTXA[0][0] + MTXA[1][1] + MTXA[2][2];

    if (trace > 0.0f)
    {
        float root = math_sqrt(trace + 1.0f);
        float scale = 0.5f / root;

        q->w = 0.5f * root;
        q->x = scale * (MTXA[2][1] - MTXA[1][2]);
        q->y = scale * (MTXA[0][2] - MTXA[2][0]);
        q->z = scale * (MTXA[1][0] - MTXA[0][1]);
    }
    else
    {
        float tmp[4];
        float root;
        int i, j, k;

        i = 0;
        if (MTXA[1][1] > MTXA[0][0])
            i = 1;
        if (MTXA[2][2] > MTXA[i][i])
            i = 2;
        j = next[i];
        k = next[j];

        root = math_sqrt(MTXA[i][i] - (MTXA[j][j] + MTXA[k][k]) + 1.0f);
        tmp[i] = 0.5f * root;
        if (root != 0.0f)
            root = 0.5f / root;

        tmp[3] = root * (MTXA[k][j] - MTXA[j][k]);
        tmp[j] = root * (MTXA[j][i] + MTXA[i][j]);
        tmp[k] = root * (MTXA[k][i] + MTXA[i][k]);

        q->x = tmp[0];
        q->y = tmp[1];
        q->z = tmp[2];
        q->w = tmp[3];
    }
}

/* ------------------------------------------------------------------ */
/* Quaternion functions                                               */
/* ------------------------------------------------------------------ */

void quat_from_axis_angle(Quat *out, Vec *axis, s16 angle)
{
    float sqLen;

    angle >>= 1;
    sqLen = vec_sq_len(axis);
    if (sqLen < FLT_EPSILON)
    {
        out->x = 0.0f;
        out->y = 0.0f;
        out->z = 0.0f;
        out->w = 1.0f;
    }
    else
    {
        float scale = math_rsqrt(sqLen) * math_sin(angle);

        out->x = axis->x * scale;
        out->y = axis->y * scale;
        out->z = axis->z * scale;
        out->w = math_sin((s16)(angle + 0x4000));  /* cos */
    }
}

void quat_mult(Quat *out, Quat *a, Quat *b)
{
    float x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
    float y = a->w * b->y + a->y * b->w + a->z * b->x - a->x * b->z;
    float z = a->w * b->z + a->z * b->w + a->x * b->y - a->y * b->x;
    float w = a->w * b->w - (a->x * b->x + a->y * b->y + a->z * b->z);

    out->x = x;
    out->y = y;
    out->z = z;
    out->w = w;
}

void quat_normalize(Quat *q)
{
    float sqMag = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    float invMag;

    if (sqMag > 0.0f)
        invMag = math_rsqrt(sqMag);
    else
        invMag = 1.0f;
    q->x *= invMag;
    q->y *= invMag;
    q->z *= invMag;
    q->w *= invMag;
}

/* Computes the rotation between two (unit) direction vectors. */
void quat_from_dirs(Quat *out, Vec *start, Vec *end)
{
    float recip;
    float x, y, z;
    float dot = start->x * end->x + start->y * end->y + start->z * end->z;

    if (dot > 0.99998998642f)
    {
        out->x = out->y = out->z = 0.0f;
        out->w = 1.0f;
    }
    else if (dot < -0.99998998642f)
    {
        /* 180 degrees: pick any axis perpendicular to start. */
        x = 0.0f;
        y = start->x;
        z = -start->y;

        if (math_sqrt(y * y + z * z) < 0.000001)
        {
            x = -start->z;
            y = 0.0f;
            z = start->x;
        }

        recip = math_rsqrt(x * x + y * y + z * z);
        out->x = x * recip;
        out->y = y * recip;
        out->z = z * recip;
        out->w = 0.0f;
    }
    else
    {
        x = start->y * end->z - start->z * end->y;
        y = start->z * end->x - start->x * end->z;
        z = start->x * end->y - start->y * end->x;

        recip = math_rsqrt(x * x + y * y + z * z);
        x *= recip;
        y *= recip;
        z *= recip;

        recip = math_sqrt(0.5f * (1.0f - dot));
        out->x = x * recip;
        out->y = y * recip;
        out->z = z * recip;
        out->w = math_sqrt(0.5f * (1.0f + dot));
    }
}

void quat_slerp(Quat *out, Quat *start, Quat *end, float t)
{
    Quat temp;
    float cosAngle = start->x * end->x + start->y * end->y
                   + start->z * end->z + start->w * end->w;
    float startMul, endMul;

    if (cosAngle < 0.0f)
    {
        cosAngle = -cosAngle;
        temp.x = -end->x;
        temp.y = -end->y;
        temp.z = -end->z;
        temp.w = -end->w;
    }
    else
    {
        temp = *end;
    }

    if (1.0f - cosAngle > 0.000001)
    {
        float angle = acosf(cosAngle);
        float s = sinf(angle);

        startMul = sinf((1.0f - t) * angle) / s;
        endMul = sinf(t * angle) / s;
    }
    else
    {
        startMul = 1.0f - t;
        endMul = t;
    }

    out->x = startMul * start->x + endMul * temp.x;
    out->y = startMul * start->y + endMul * temp.y;
    out->z = startMul * start->z + endMul * temp.z;
    out->w = startMul * start->w + endMul * temp.w;
}

/* ------------------------------------------------------------------ */
/* g_math_unk*                                                        */
/* ------------------------------------------------------------------ */

/* Builds the quaternion that rotates by |a| (interpreted as an angle in
 * radians times `t`... see SMB1's u_math_unk8) about the axis cross(a, b).
 * SMB1: u_math_unk8(Quaternion *q, Vec *a, Vec *b, float t); SMB2's decomp
 * declares the float earlier in the list, which is ABI-identical on PPC. */
void g_math_unk8(Quat *q, float t, Vec *a, Vec *b)
{
    Vec axis;
    float len;
    s16 angle;

    vec_cross(a, b, &axis);
    vec_normalize_len(&axis);
    len = math_sqrt(vec_sq_len(a));
    if (len > MU_PI * t - FLT_EPSILON)
        len = MU_PI * t - FLT_EPSILON;
    angle = (s16)(int)((0.159154936671f * len / t) * 65536.0f);
    quat_from_axis_angle(q, &axis, (s16)-angle);
}

/* Quaternion from axis and angle, where the angle is a float in radians. */
void g_math_unk9_smth_w_quats(Quat *out, Vec *axis, float angle)
{
    double half = angle / 2.0;
    float sqLen = vec_sq_len(axis);

    if (sqLen < FLT_EPSILON)
    {
        out->x = 0.0f;
        out->y = 0.0f;
        out->z = 0.0f;
        out->w = 1.0f;
    }
    else
    {
        float scale = math_rsqrt(sqLen) * (float)sin(half);

        out->x = axis->x * scale;
        out->y = axis->y * scale;
        out->z = axis->z * scale;
        out->w = (float)cos(half);
    }
}

/* Builds a "look at" view matrix in mtxA: camera at `eye`, -Z axis pointing
 * at `target`, with `up` as the up hint.  Used by bg_spa2.c to render the
 * spotlight shadow map (mtxa_to_mtx() right after, used as view matrix). */
void g_math_unk13(Vec *eye, Vec *up, Vec *target)
{
    Vec xAxis, yAxis, zAxis;

    /* Camera +Z points from the target back toward the eye. */
    zAxis.x = eye->x - target->x;
    zAxis.y = eye->y - target->y;
    zAxis.z = eye->z - target->z;
    vec_normalize_len(&zAxis);

    vec_cross(up, &zAxis, &xAxis);
    vec_normalize_len(&xAxis);
    vec_cross(&zAxis, &xAxis, &yAxis);

    MTXA[0][0] = xAxis.x;
    MTXA[0][1] = xAxis.y;
    MTXA[0][2] = xAxis.z;
    MTXA[0][3] = -(xAxis.x * eye->x + xAxis.y * eye->y + xAxis.z * eye->z);

    MTXA[1][0] = yAxis.x;
    MTXA[1][1] = yAxis.y;
    MTXA[1][2] = yAxis.z;
    MTXA[1][3] = -(yAxis.x * eye->x + yAxis.y * eye->y + yAxis.z * eye->z);

    MTXA[2][0] = zAxis.x;
    MTXA[2][1] = zAxis.y;
    MTXA[2][2] = zAxis.z;
    MTXA[2][3] = -(zAxis.x * eye->x + zAxis.y * eye->y + zAxis.z * eye->z);
}

/* Transforms a point by mtxA and projects it to normalized screen space
 * (used by the minimap); matches SMB1's u_math_unk16. */
void g_math_unk16(Vec *src, Vec *dst, float tanHalfFov)
{
    mtxa_tf_point(src, dst);
    if (fabsf(dst->z) > FLT_EPSILON)
    {
        float scale = -1.0f / (dst->z * tanHalfFov);

        dst->x = 0.75f * dst->x * scale;
        dst->y = dst->y * scale;
    }
    else
    {
        dst->x = 10000.0f;
        dst->y = 10000.0f;
    }
}

/* DOL 0x80064558: transforms a point by mtxA and projects it to 640x480
 * screen pixels (Monkey Fight's HUD labels); points at the eye plane give
 * 10000 in both. */
void g_math_unk15(Vec *src, Vec *dst, float tanHalfFov)
{
    mtxa_tf_point(src, dst);
    if (fabsf(dst->z) > FLT_EPSILON)
    {
        float scale = 240.0f / (dst->z * tanHalfFov);

        dst->x = 320.0f + -dst->x * scale;
        dst->y = 240.0f + dst->y * scale;
    }
    else
    {
        dst->x = 10000.0f;
        dst->y = 10000.0f;
    }
}

/* ------------------------------------------------------------------ */
/* GC SDK (Dolphin) matrix functions                                  */
/* ------------------------------------------------------------------ */

void PSMTXIdentity(Mtx m)
{
    mtx_from_identity(m);
}

void PSMTXScale(Mtx m, float x, float y, float z)
{
    m[0][0] = x;    m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = y;    m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = z;    m[2][3] = 0.0f;
}

void PSVECSubtract(Vec *a, Vec *b, Vec *dst)
{
    float x = a->x - b->x;
    float y = a->y - b->y;
    float z = a->z - b->z;

    dst->x = x;
    dst->y = y;
    dst->z = z;
}

/* GC-style perspective frustum: clip-space z/w ranges from -1 (near) to 0
 * (far), matching the SDK's C_MTXFrustum. */
void C_MTXFrustum(Mtx44 m, float t, float b, float l, float r, float n, float f)
{
    float tmp;

    tmp = 1.0f / (r - l);
    m[0][0] = (2.0f * n) * tmp;
    m[0][1] = 0.0f;
    m[0][2] = (r + l) * tmp;
    m[0][3] = 0.0f;

    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = (2.0f * n) * tmp;
    m[1][2] = (t + b) * tmp;
    m[1][3] = 0.0f;

    tmp = 1.0f / (f - n);
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -n * tmp;
    m[2][3] = -(f * n) * tmp;

    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

/* ---- helpers referenced by the minigame RELs (race2/pilot2/bowling/boat/
 * dogfight/baseball); semantics from the DOL asm ---- */
void PSVECAdd(Vec *a, Vec *b, Vec *dst)
{
    dst->x = a->x + b->x; dst->y = a->y + b->y; dst->z = a->z + b->z;
}

void PSVECScale(Vec *src, Vec *dst, float s)
{
    dst->x = src->x * s; dst->y = src->y * s; dst->z = src->z * s;
}

/* 0x80061D74: *sqrtOut = sqrt(x), returns 1/sqrt(x) */
float math_sqrt_rsqrt(float x, float *sqrtOut)
{
    float r;
    if (x <= 0.0f) { *sqrtOut = 0.0f; return 0.0f; }
    r = sqrtf(x);
    *sqrtOut = r;
    return 1.0f / r;
}

/* 0x800620A8: asin as s16 angle */
s16 g_math_unk3(float x)
{
    float s;
    if (fabsf(x) > 1.0f)
        return x > 0.0f ? 0x4000 : -0x4000;
    s = math_sqrt(1.0f - x * x);
    if (s > FLT_EPSILON)
        return math_atan(x / s);
    return x > 0.0f ? 0x4000 : -0x4000;
}

/* 0x80062144: acos as s16 angle */
s16 g_math_unk4(float x)
{
    if (fabsf(x) > 1.0f)
        return x > 0.0f ? 0 : -0x8000;
    return (s16)(0x4000 - g_math_unk3(x));
}

/* 0x800637DC: returns rotation angle (radians), normalized axis in *axis */
float quat_to_axis_angle(Quat *q, Vec *axis)
{
    float w = q->w;
    if (w > 1.0f) w = 1.0f;
    if (w < -1.0f) w = -1.0f;
    axis->x = q->x; axis->y = q->y; axis->z = q->z;
    vec_normalize_len(axis);
    return 2.0f * acosf(w);
}

/* 0x80063194 (Monkey Billiards): quaternion inverse, conjugate / |q|^2,
 * identity for a near-zero quaternion (SMB1 u_math_unk6) */
void g_math_unk6(Quat *q)
{
    float len = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;

    if (len < FLT_EPSILON) {
        q->x = q->y = q->z = 0.0f;
        q->w = 1.0f;
    } else {
        float r = 1.0f / len;
        q->x = -q->x * r;
        q->y = -q->y * r;
        q->z = -q->z * r;
        q->w = q->w * r;
    }
}

/* 0x80063D5C / 0x80063D9C (Monkey Billiards): a quaternion's vector part
 * through MTXA (rigid inverse), w copied (SMB1 u_math_unk10/11) */
void g_math_unk10(Quat *a, Quat *b)
{
    Vec v = { a->x, a->y, a->z };
    float w = a->w;
    mtxa_tf_vec(&v, &v);
    b->x = v.x; b->y = v.y; b->z = v.z; b->w = w;
}

void g_math_unk11(Quat *a, Quat *b)
{
    Vec v = { a->x, a->y, a->z };
    float w = a->w;
    mtxa_rigid_inv_tf_vec(&v, &v);
    b->x = v.x; b->y = v.y; b->z = v.z; b->w = w;
}

/* 0x8006322C: tangential displacement of point `in` (radius r) under quat */
void g_math_unk7(Quat *quat, float radius, Vec *out, Vec *in)
{
    Vec a;
    float angle = quat_to_axis_angle(quat, &a);
    float k;

    if (fabsf(a.x * in->x + a.y * in->y + a.z * in->z) >= 0.9999999) {
        out->x = out->y = out->z = 0.0f;
        return;
    }
    k = radius * angle;
    out->x = (a.y * in->z - a.z * in->y) * k;
    out->y = (a.z * in->x - a.x * in->z) * k;
    out->z = (a.x * in->y - a.y * in->x) * k;
}

/* 0x80064820: project point through MTXA to normalized screen coords */
/* DOL 0x80064738: like g_math_unk19, but to 640x480 screen pixels; points
 * behind/at the eye plane or off screen give 10000 in both. */
void g_math_unk18(Vec *pos, float *out, float tanHalfFov, float aspect)
{
    mtxa_tf_point(pos, (Vec *)out);
    if (fabs(out[2]) > 1.1920928955078125e-07) {
        float s = 240.0f / (out[2] * tanHalfFov);
        out[0] = 320.0f + (1.3333334f * (-out[0] * s)) / aspect;
        out[1] = 240.0f + out[1] * s;
        if (out[0] < 640.0f && out[0] > 0.0f && out[1] < 480.0f && out[1] > 0.0f)
            return;
    }
    out[0] = out[1] = 10000.0f;
}

void g_math_unk19(Vec *pos, float *out, float tanHalfFov, float aspect)
{
    mtxa_tf_point(pos, (Vec *)out);
    if (fabsf(out[2]) > FLT_EPSILON) {
        float b = -1.0f / (out[2] * tanHalfFov);
        out[0] = b * ((1.0f / aspect) * out[0]);
        out[1] *= b;
    } else {
        out[0] = out[1] = 10000.0f;
    }
}

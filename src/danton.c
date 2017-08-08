/*
 * Copyright (C) 2017 Université Clermont Auvergne, CNRS/IN2P3, LPC
 * Author: Valentin NIESS (niess@in2p3.fr)
 *
 * This software is a C99 executable dedicated to the sampling of decaying
 * taus from ultra high energy neutrinos.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

/* Standard library includes. */
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The various APIs. */
#include "alouette.h"
#include "danton.h"
#include "ent.h"
#include "pumas.h"

/* The spherical Earth radius, in m. */
#define EARTH_RADIUS 6371.E+03

/* The radius of the geostationary orbit, in m. */
#define GEO_ORBIT 42164E+03

/* Avogadro's number. */
#define PHYS_NA 6.022E+23

/* Biasing factor for backward tau decays. */
#define DECAY_BIAS 6.

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Handle for ENT Physic. */
static struct ent_physics * physics = NULL;

/* The tau lepton mass, in GeV / c^2. */
static double tau_mass;

/* The tau lifetime, in m / c. */
static double tau_ctau0;

/* Density according to the Preliminary Earth Model (PEM). */
static double pem_model0(double r, double * density)
{
        const double x = r / EARTH_RADIUS;
        const double a2 = -8.8381E+03;
        *density = 13.0885E+03 + a2 * x * x;
        const double xg = (x <= 5E-02) ? 5E-02 : x;
        return 0.01 * EARTH_RADIUS / fabs(2. * a2 * xg);
}

static double pem_model1(double r, double * density)
{
        const double x = r / EARTH_RADIUS;
        const double a = 1.2638E+03;
        *density = 12.58155E+03 + x * (-a + x * (-3.6426E+03 - x * 5.5281E+03));
        return 0.01 * EARTH_RADIUS / a;
}

static double pem_model2(double r, double * density)
{
        const double x = r / EARTH_RADIUS;
        const double a = 6.4761E+03;
        *density = 7.9565E+03 + x * (-a + x * (5.5283E+03 - x * 3.0807E+03));
        return 0.01 * EARTH_RADIUS / a;
}

static double pem_model3(double r, double * density)
{
        const double x = r / EARTH_RADIUS;
        const double a = 1.4836E+03;
        *density = 5.3197E+03 - a * x;
        return 0.01 * EARTH_RADIUS / a;
}

static double pem_model4(double r, double * density)
{
        const double x = r / EARTH_RADIUS;
        const double a = 8.0298E+03;
        *density = 11.2494E+03 - a * x;
        return 0.01 * EARTH_RADIUS / a;
}

static double pem_model5(double r, double * density)
{
        const double x = r / EARTH_RADIUS;
        const double a = 3.8045E+03;
        *density = 7.1089E+03 - a * x;
        return 0.01 * EARTH_RADIUS / a;
}

static double pem_model6(double r, double * density)
{
        const double x = r / EARTH_RADIUS;
        const double a = 0.6924E+03;
        *density = 2.691E+03 + a * x;
        return 0.01 * EARTH_RADIUS / a;
}

static double pem_model7(double r, double * density)
{
        *density = 2.9E+03;
        return 0.;
}

static double pem_model8(double r, double * density)
{
        *density = 2.6E+03;
        return 0.;
}

static double pem_model9(double r, double * density)
{
        *density = 1.02E+03;
        return 0.;
}

/* The U.S. standard atmosphere model. */
#define USS_MODEL(INDEX, B, C)                                                 \
        static double uss_model##INDEX(double r, double * density)             \
        {                                                                      \
                *density = B / C * exp(-(r - EARTH_RADIUS) / C);               \
                return 0.01 * C;                                               \
        }

USS_MODEL(0, 12226.562, 9941.8638)
USS_MODEL(1, 11449.069, 8781.5355)
USS_MODEL(2, 13055.948, 6361.4304)
USS_MODEL(3, 5401.778, 7721.7016)

/* Outer space density model. */
static double space_model0(double r, double * density)
{
        *density = 1.E-21; /* ~10^6 H per m^-3. */
        return 0.;
}

/* Container for contextual simulation data. */
struct simulation_context {
        /* Public API data. */
        struct danton_context api;

        /* Sub contexts for the transport engines. */
        struct pumas_context * pumas;
        struct ent_context ent;

        /* The lower (upper) energy bound under (above) which all particles are
         * killed.
         */
        double energy_cut;

        /* Flag for the dumping of the primary state. */
        int primary_dumped;

        /* Flag to check if the neutrino flux is requested. */
        int flux_neutrino;

        /* Data for the Mersenne Twister PRNG. */
        struct {
#define MT_PERIOD 624
                int index;
                unsigned long data[MT_PERIOD];
        } random_mt;
};

/* Get the full simulation context from the PUMAS' one. */
static struct simulation_context * pumas2context(struct pumas_context * context)
{
        return (struct simulation_context *)context->user_data;
}

/* Get the full simulation context from the ENT's one. */
static struct simulation_context * ent2context(struct ent_context * context)
{
        char * p = (char *)context;
        p -= offsetof(struct simulation_context, ent);
        return (struct simulation_context *)p;
}

/* Generic Monte-Carlo state with stepping data. */
struct generic_state {
        union {
                struct ent_state ent;
                struct pumas_state pumas;
        } base;
        struct simulation_context * context;
        int medium;
        double density;
        double r;
        int is_tau;
        int is_inside;
        int has_crossed;
        int cross_count;
};

/* Density callbacks for ENT. */
#define DENSITY(MODEL, INDEX)                                                  \
        static double density_##MODEL##INDEX(struct ent_medium * medium,       \
            struct ent_state * state, double * density)                        \
        {                                                                      \
                struct generic_state * s = (struct generic_state *)state;      \
                const double step = MODEL##_model##INDEX(s->r, density);       \
                s->density = *density;                                         \
                return step;                                                   \
        }

DENSITY(pem, 0)
DENSITY(pem, 1)
DENSITY(pem, 2)
DENSITY(pem, 3)
DENSITY(pem, 4)
DENSITY(pem, 5)
DENSITY(pem, 6)
DENSITY(pem, 7)
DENSITY(pem, 8)
DENSITY(pem, 9)
DENSITY(uss, 0)
DENSITY(uss, 1)
DENSITY(uss, 2)
DENSITY(uss, 3)
DENSITY(space, 0)

/* Local callbacks for PUMAS. */
#define LOCALS(MODEL, INDEX)                                                   \
        static double locals_##MODEL##INDEX(struct pumas_medium * medium,      \
            struct pumas_state * state, struct pumas_locals * locals)          \
        {                                                                      \
                struct generic_state * s = (struct generic_state *)state;      \
                const double step =                                            \
                    MODEL##_model##INDEX(s->r, &locals->density);              \
                memset(locals->magnet, 0x0, sizeof(locals->magnet));           \
                s->density = locals->density;                                  \
                return step;                                                   \
        }

LOCALS(pem, 0)
LOCALS(pem, 1)
LOCALS(pem, 2)
LOCALS(pem, 3)
LOCALS(pem, 4)
LOCALS(pem, 5)
LOCALS(pem, 6)
LOCALS(pem, 7)
LOCALS(pem, 8)
LOCALS(pem, 9)
LOCALS(uss, 0)
LOCALS(uss, 1)
LOCALS(uss, 2)
LOCALS(uss, 3)
LOCALS(space, 0)

/* Generic medium callback. */
static double medium(const double * position, const double * direction,
    struct generic_state * state)
{
        state->medium = -1;
        double step = 0.;

        const double r2 = position[0] * position[0] +
            position[1] * position[1] + position[2] * position[2];
        if (r2 > GEO_ORBIT * GEO_ORBIT) return step;
        const double r = sqrt(r2);
        state->r = r;

        if (!state->context->api.decay && (state->has_crossed >= 0)) {
                /* Check the flux boundary in forward MC. */
                const double zi = state->context->api.sampler->altitude[0];
                const double rf = EARTH_RADIUS + zi;
                if (state->is_inside < 0)
                        state->is_inside = (r < rf) ? 1 : 0;
                else if ((state->is_inside && (r >= rf)) ||
                    (!state->is_inside && (r <= rf))) {
                        state->has_crossed = 1;
                        return step;
                }
        };

        const double ri[16] = { 1221.5E+03, 3480.E+03, 5701.E+03, 5771.E+03,
                5971.E+03, 6151.E+03, 6346.6E+03, 6356.E+03, 6368.E+03,
                EARTH_RADIUS, EARTH_RADIUS + 4.E+03, EARTH_RADIUS + 1.E+04,
                EARTH_RADIUS + 4.E+04, EARTH_RADIUS + 1.E+05, GEO_ORBIT,
                2 * GEO_ORBIT };

        /* Kill neutrinos that exit the atmosphere.  */
        if ((!state->is_tau) && (state->r > ri[13])) return step;

        int i;
        for (i = 0; i < sizeof(ri) / sizeof(*ri) - 1; i++) {
                if (r <= ri[i]) {
                        state->medium = i;

                        /* Outgoing intersection. */
                        const double b = position[0] * direction[0] +
                            position[1] * direction[1] +
                            position[2] * direction[2];
                        const double d2 = b * b + ri[i] * ri[i] - r * r;
                        const double d = (d2 <= 0.) ? 0. : sqrt(d2);
                        step = d - b;

                        if ((i > 0) && (b < 0.)) {
                                /* This is a downgoing trajectory. Let
                                 * us compute the intersection with the lower
                                 * radius.
                                 */
                                const double r1 = ri[i - 1];
                                const double d2 = b * b + r1 * r1 - r * r;
                                if (d2 > 0.) {
                                        const double d = sqrt(d2);
                                        double s = -b - d;
                                        if ((s > 0.) && (s < step)) step = s;
                                }
                        }
                        if (step < 1E-03) step = 1E-03;
                        break;
                }
        }
        return step;
}

/* Media table for ENT. */
#define ZR 13.
#define AR 26.
#define ZW 10.
#define AW 18.
#define ZA 7.32
#define AA 14.72

static struct ent_medium media_ent[] = { { ZR, AR, &density_pem0 },
        { ZR, AR, &density_pem1 }, { ZR, AR, &density_pem2 },
        { ZR, AR, &density_pem3 }, { ZR, AR, &density_pem4 },
        { ZR, AR, &density_pem5 }, { ZR, AR, &density_pem6 },
        { ZR, AR, &density_pem7 }, { ZR, AR, &density_pem8 },
        { ZW, AW, &density_pem9 }, { ZA, AA, &density_uss0 },
        { ZA, AA, &density_uss1 }, { ZA, AA, &density_uss2 },
        { ZA, AA, &density_uss3 }, { ZA, AA, &density_space0 } };

/* Medium callback encapsulation for ENT. */
static double medium_ent(struct ent_context * context, struct ent_state * state,
    struct ent_medium ** medium_ptr)
{
        double direction[3];
        if (context->ancestor) {
                direction[0] = -state->direction[0];
                direction[1] = -state->direction[1];
                direction[2] = -state->direction[2];
        } else {
                direction[0] = state->direction[0];
                direction[1] = state->direction[1];
                direction[2] = state->direction[2];
        }
        struct generic_state * g = (struct generic_state *)state;
        const double step = medium(state->position, direction, g);
        if (g->medium >= 0)
                *medium_ptr = media_ent + g->medium;
        else
                *medium_ptr = NULL;
        return step;
}

#undef ZR
#undef AR
#undef ZA
#undef AA

/* Media table for PUMAS. */
static struct pumas_medium media_pumas[] = { { 0, &locals_pem0 },
        { 0, &locals_pem1 }, { 0, &locals_pem2 }, { 0, &locals_pem3 },
        { 0, &locals_pem4 }, { 0, &locals_pem5 }, { 0, &locals_pem6 },
        { 0, &locals_pem7 }, { 0, &locals_pem8 }, { 1, &locals_pem9 },
        { 2, &locals_uss0 }, { 2, &locals_uss1 }, { 2, &locals_uss2 },
        { 2, &locals_uss3 }, { 2, &locals_space0 } };

/* Medium callback encapsulation for PUMAS. */
double medium_pumas(struct pumas_context * context, struct pumas_state * state,
    struct pumas_medium ** medium_ptr)
{
        double direction[3];
        if (context->forward) {
                direction[0] = state->direction[0];
                direction[1] = state->direction[1];
                direction[2] = state->direction[2];
        } else {
                direction[0] = -state->direction[0];
                direction[1] = -state->direction[1];
                direction[2] = -state->direction[2];
        }
        struct generic_state * g = (struct generic_state *)state;
        const double step = medium(state->position, direction, g);
        if (g->medium >= 0)
                *medium_ptr = media_pumas + g->medium;
        else
                *medium_ptr = NULL;
        return step;
}

/* Initialise the PRNG random seed. */
static void random_initialise(struct simulation_context * context)
{
        /* Get a seed from /dev/urandom*/
        unsigned long seed;
        static const char * urandom = "/dev/urandom";
        FILE * fp = fopen(urandom, "rb");
        if (fp == NULL) goto error;
        if (fread(&seed, sizeof(long), 1, fp) <= 0) {
                fclose(fp);
                goto error;
        }
        fclose(fp);

        /* Set the Mersenne Twister initial state. */
        context->random_mt.data[0] = seed & 0xffffffffUL;
        int j;
        for (j = 1; j < MT_PERIOD; j++) {
                context->random_mt.data[j] =
                    (1812433253UL *
                            (context->random_mt.data[j - 1] ^
                                (context->random_mt.data[j - 1] >> 30)) +
                        j);
                context->random_mt.data[j] &= 0xffffffffUL;
        }
        context->random_mt.index = MT_PERIOD;

        return;
error:
        fprintf(
            stderr, "danton: could not Initialise PRNG from %s.\n", urandom);
        exit(EXIT_FAILURE);
}

/* Uniform pseudo random distribution over [0,1] from a Mersenne Twister. */
static double random_uniform01(struct simulation_context * context)
{
        /* Check the buffer. */
        if (context->random_mt.index < MT_PERIOD - 1) {
                context->random_mt.index++;
        } else {
                /* Update the MT state. */
                const int M = 397;
                const unsigned long UPPER_MASK = 0x80000000UL;
                const unsigned long LOWER_MASK = 0x7fffffffUL;
                static unsigned long mag01[2] = { 0x0UL, 0x9908b0dfUL };
                unsigned long y;
                int kk;
                for (kk = 0; kk < MT_PERIOD - M; kk++) {
                        y = (context->random_mt.data[kk] & UPPER_MASK) |
                            (context->random_mt.data[kk + 1] & LOWER_MASK);
                        context->random_mt.data[kk] =
                            context->random_mt.data[kk + M] ^ (y >> 1) ^
                            mag01[y & 0x1UL];
                }
                for (; kk < MT_PERIOD - 1; kk++) {
                        y = (context->random_mt.data[kk] & UPPER_MASK) |
                            (context->random_mt.data[kk + 1] & LOWER_MASK);
                        context->random_mt.data[kk] =
                            context->random_mt.data[kk + (M - MT_PERIOD)] ^
                            (y >> 1) ^ mag01[y & 0x1UL];
                }
                y = (context->random_mt.data[MT_PERIOD - 1] & UPPER_MASK) |
                    (context->random_mt.data[0] & LOWER_MASK);
                context->random_mt.data[MT_PERIOD - 1] =
                    context->random_mt.data[M - 1] ^ (y >> 1) ^
                    mag01[y & 0x1UL];
                context->random_mt.index = 0;
        }

        /* Tempering. */
        unsigned long y = context->random_mt.data[context->random_mt.index];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680UL;
        y ^= (y << 15) & 0xefc60000UL;
        y ^= (y >> 18);

        /* Convert to a floating point and return. */
        return y * (1.0 / 4294967295.0);
}

double random_pumas(struct pumas_context * context)
{
        struct simulation_context * c = pumas2context(context);
        return random_uniform01(c);
}

double random_ent(struct ent_context * context)
{
        struct simulation_context * c = ent2context(context);
        return random_uniform01(c);
}

/* Set a neutrino state from a tau decay product. */
static void copy_neutrino(struct simulation_context * context,
    const struct pumas_state * tau, int pid, const double * momentum,
    struct ent_state * neutrino, double * direction)
{
        neutrino->pid = pid;
        neutrino->energy = sqrt(momentum[0] * momentum[0] +
            momentum[1] * momentum[1] + momentum[2] * momentum[2]);
        memcpy(neutrino->position, tau->position, sizeof(neutrino->position));
        if (context->api.longitudinal) {
                neutrino->direction[0] = direction[0];
                neutrino->direction[1] = direction[1];
                neutrino->direction[2] = direction[2];
        } else {
                neutrino->direction[0] = momentum[0] / neutrino->energy;
                neutrino->direction[1] = momentum[1] / neutrino->energy;
                neutrino->direction[2] = momentum[2] / neutrino->energy;
        }
        neutrino->distance = tau->distance;
        neutrino->grammage = tau->grammage;
        neutrino->weight = tau->weight;
}

/* Get or create the output stream. */
static FILE * output_create(struct simulation_context * context)
{
        if (context->api.output == NULL) return stdout;
        return fopen(context->api.output, "w+");
}

/* Open the output stream and append to it. */
static FILE * output_open(struct simulation_context * context)
{
        if (context->api.output == NULL) return stdout;
        return fopen(context->api.output, "a");
}

/* Close the output stream. */
static void output_close(struct simulation_context * context, FILE * stream)
{
        if (context->api.output != NULL) fclose(stream);
}

/* Utility functions for formating the results to the output stream. */
static void format_ancestor(struct simulation_context * context, long eventid,
    const struct ent_state * ancestor)
{
        FILE * stream = output_open(context);
        fprintf(stream, "%10ld %4d %12.5lE %12.5lE %12.5lE %12.5lE %13.3lf "
                        "%13.3lf %13.3lf %12.5lE\n",
            eventid + 1, ancestor->pid, ancestor->energy,
            ancestor->direction[0], ancestor->direction[1],
            ancestor->direction[2], ancestor->position[0],
            ancestor->position[1], ancestor->position[2], ancestor->weight);
        output_close(context, stream);
}

static void format_tau(struct simulation_context * context, int generation,
    int pid, const struct pumas_state * production,
    const struct pumas_state * decay)
{
        FILE * stream = output_open(context);
        fprintf(stream, "%10d %4d %12.5lE %12.5lE %12.5lE %12.5lE %13.3lf "
                        "%13.3lf %13.3lf\n",
            generation, pid, production->kinetic + tau_mass,
            production->direction[0], production->direction[1],
            production->direction[2], production->position[0],
            production->position[1], production->position[2]);
        fprintf(stream, "%10c %4c %12.5lE %12.5lE %12.5lE %12.5lE %13.3lf "
                        "%13.3lf %13.3lf\n",
            ' ', ' ', decay->kinetic + tau_mass, decay->direction[0],
            decay->direction[1], decay->direction[2], decay->position[0],
            decay->position[1], decay->position[2]);
        output_close(context, stream);
}

static void format_neutrino(struct simulation_context * context, int generation,
    const struct ent_state * neutrino)
{
        FILE * stream = output_open(context);
        fprintf(stream, "%10d %4d %12.5lE %12.5lE %12.5lE %12.5lE %13.3lf "
                        "%13.3lf %13.3lf\n",
            generation, neutrino->pid, neutrino->energy, neutrino->direction[0],
            neutrino->direction[1], neutrino->direction[2],
            neutrino->position[0], neutrino->position[1],
            neutrino->position[2]);
        output_close(context, stream);
}

static void format_decay_product(
    struct simulation_context * context, int pid, const double momentum[3])
{
        FILE * stream = output_open(context);
        fprintf(stream, "%10c %4d %12c %12.5lE %12.5lE %12.5lE\n", ' ', pid,
            ' ', momentum[0], momentum[1], momentum[2]);
        output_close(context, stream);
}

static void format_grammage(
    struct simulation_context * context, double cos_theta, double grammage)
{
        FILE * stream = output_open(context);
        if (context->api.forward)
                fprintf(stream, "%12.5lE %12.5lE\n", cos_theta, grammage);
        else {
                const double elevation = 90. - acos(cos_theta) * 180. / M_PI;
                fprintf(stream, "%12.5lf %12.5lE\n", elevation, grammage);
        }
        output_close(context, stream);
}

/* Print the header of the output file. */
static void print_header_decay(FILE * stream)
{
        fprintf(stream,
            "    Event   PID    Energy             Direction or "
            "Momentum                         Position               "
            "      Weight\n                    (GeV)                "
            " (1 or GeV/c)                                 (m)\n     "
            "                           ux or Px     uy or Py    "
            "uz or Pz         X             Y             Z\n");
}

static void print_header_grammage(int forward, FILE * stream)
{
        if (forward)
                fprintf(stream,
                    "  cos(theta)    Grammage\n                (kg/m^2)\n");
        else
                fprintf(stream,
                    "   elevation    Grammage\n     (deg)      (kg/m^2)\n");
}

/* Forward transport routine, recursive. */
static void transport_forward(struct simulation_context * context,
    struct ent_state * neutrino, long eventid, int generation,
    const struct ent_state * ancestor)
{
        if ((neutrino->pid != ENT_PID_NU_BAR_E) &&
            (abs(neutrino->pid) != ENT_PID_NU_TAU))
                return;

        /* Backup the initial direction if the transverse transport is
         * disabled.
         */
        double direction[3];
        if (context->api.longitudinal)
                memcpy(direction, neutrino->direction, sizeof(direction));

        struct danton_sampler * const sampler = context->api.sampler;
        struct ent_state product;
        enum ent_event event;
        for (;;) {
                /* Neutrino transport with ENT. */
                ent_transport(
                    physics, &context->ent, neutrino, &product, &event);
                if (neutrino->energy <= context->energy_cut + FLT_EPSILON)
                        break;
                if (context->flux_neutrino && (event == ENT_EVENT_EXIT)) {
                        /* Check for a flux crossing condition. */
                        struct generic_state * g_state =
                            (struct generic_state *)neutrino;
                        if (g_state->has_crossed) {
                                g_state->cross_count++;
                                if (g_state->cross_count == 2) {
                                        if (!context->primary_dumped) {
                                                format_ancestor(
                                                    context, eventid, ancestor);
                                                context->primary_dumped = 1;
                                        }
                                        format_neutrino(
                                            context, generation, neutrino);
                                        break;
                                } else {
                                        g_state->is_inside = -1;
                                        g_state->has_crossed = 0;
                                        continue;
                                }
                        }
                }
                if (event == ENT_EVENT_EXIT) break;
                if (context->api.longitudinal) {
                        memcpy(neutrino->direction, direction,
                            sizeof(neutrino->direction));
                        memcpy(product.direction, direction,
                            sizeof(product.direction));
                }
                if (abs(neutrino->pid) == ENT_PID_TAU) {
                        /* Exchange the lepton and the product state. */
                        struct ent_state tmp;
                        memcpy(&tmp, neutrino, sizeof(tmp));
                        memcpy(neutrino, &product, sizeof(*neutrino));
                        memcpy(&product, &tmp, sizeof(product));
                }

                if (abs(product.pid) == ENT_PID_TAU) {
                        /* Tau transport with PUMAS. */
                        const double charge = (product.pid > 0) ? -1. : 1.;
                        const double kinetic = product.energy - tau_mass;
                        struct generic_state tau_data = {
                                .base.pumas = { charge, kinetic,
                                    product.distance, product.grammage, 0.,
                                    product.weight, .decayed = 0 },
                                .context = context,
                                .medium = -1,
                                .density = 0.,
                                .r = 0.,
                                .is_tau = 1,
                                .is_inside = -1,
                                .has_crossed =
                                    (context->flux_neutrino) ? -1 : 0,
                                .cross_count = 0
                        };
                        struct pumas_state * tau = &tau_data.base.pumas;
                        memcpy(&tau->position, &product.position,
                            sizeof(tau->position));
                        memcpy(&tau->direction, &product.direction,
                            sizeof(tau->direction));
                        struct pumas_state tau_prod;
                        memcpy(&tau_prod, tau, sizeof(tau_prod));
                        pumas_transport(context->pumas, tau);
                        if (tau->decayed) {
                                /* Tau decay with ALOUETTE/TAUOLA. */
                                const double p = sqrt(tau->kinetic *
                                    (tau->kinetic + 2. * tau_mass));
                                double momentum[3] = { p * tau->direction[0],
                                        p * tau->direction[1],
                                        p * tau->direction[2] };
                                int trials;
                                for (trials = 0; trials < 20; trials++) {
                                        if (alouette_decay(product.pid,
                                                momentum, tau->direction) ==
                                            ALOUETTE_RETURN_SUCCESS)
                                                break;
                                }
                                int pid, nprod = 0;
                                struct generic_state nu_e_data, nu_t_data;
                                memset(&nu_e_data, 0x0, sizeof(nu_e_data));
                                memset(&nu_t_data, 0x0, sizeof(nu_t_data));
                                struct ent_state *nu_e = NULL, *nu_t = NULL;
                                while (alouette_product(&pid, momentum) ==
                                    ALOUETTE_RETURN_SUCCESS) {
                                        if (abs(pid) == 16) {
                                                /* Update the neutrino state
                                                 * with the nu_tau daughter.
                                                 */
                                                nu_t = &nu_t_data.base.ent;
                                                copy_neutrino(context, tau, pid,
                                                    momentum, nu_t, direction);
                                                continue;
                                        } else if (pid == -12) {
                                                nu_e = &nu_e_data.base.ent;
                                                copy_neutrino(context, tau, pid,
                                                    momentum, nu_e, direction);
                                                continue;
                                        } else if (!context->api.decay ||
                                            (pid == 12) || (abs(pid) == 13) ||
                                            (abs(pid) == 14))
                                                continue;

                                        /* Log the decay if in air. */
                                        if (tau_data.medium < 10) continue;
                                        if (nprod == 0) {
                                                if (!context->primary_dumped) {
                                                        format_ancestor(context,
                                                            eventid, ancestor);
                                                        context
                                                            ->primary_dumped =
                                                            1;
                                                }
                                                format_tau(context, generation,
                                                    product.pid, &tau_prod,
                                                    tau);
                                        }
                                        format_decay_product(
                                            context, pid, momentum);
                                        nprod++;
                                }
                                generation++;

                                /* Process any additional nu_e~ or nu_tau. */
                                if (nu_e != NULL) {
                                        nu_e_data.context = context;
                                        if (context->flux_neutrino) {
                                                nu_e_data.is_inside = -1;
                                                nu_e_data.has_crossed = 0;
                                                nu_e_data.cross_count =
                                                    (tau_data.r <=
                                                        EARTH_RADIUS +
                                                            sampler
                                                                ->altitude[0] +
                                                            FLT_EPSILON) ?
                                                    1 :
                                                    0;
                                        } else {
                                                nu_e_data.has_crossed = -1;
                                        }
                                        transport_forward(context, nu_e,
                                            eventid, generation, ancestor);
                                }
                                if (nu_t != NULL) {
                                        nu_t_data.context = context;
                                        if (context->flux_neutrino) {
                                                nu_t_data.is_inside = -1;
                                                nu_t_data.has_crossed = 0;
                                                nu_t_data.cross_count =
                                                    (tau_data.r <=
                                                        EARTH_RADIUS +
                                                            sampler
                                                                ->altitude[0] +
                                                            FLT_EPSILON) ?
                                                    1 :
                                                    0;
                                        } else {
                                                nu_t_data.has_crossed = -1;
                                        }
                                        transport_forward(context, nu_t,
                                            eventid, generation, ancestor);
                                }
                        } else if (tau_data.has_crossed == 1) {
                                if (!context->primary_dumped) {
                                        format_ancestor(
                                            context, eventid, ancestor);
                                        context->primary_dumped = 1;
                                }
                                format_tau(context, generation, product.pid,
                                    &tau_prod, tau);
                        }
                }
                if ((neutrino->pid != ENT_PID_NU_BAR_E) &&
                    (abs(neutrino->pid) != ENT_PID_NU_TAU))
                        break;
        }
}

static double ancestor_tau(
    struct ent_context * context, struct ent_state * state)
{
        struct generic_state * g = (struct generic_state *)state;
        return 1.63E-17 * pow(state->energy, 1.363) * g->density;
}

/* Ancestor callback for ENT. */
static double ancestor_cb(struct ent_context * context, enum ent_pid ancestor,
    struct ent_state * daughter)
{
        if (daughter->pid == ENT_PID_NU_BAR_E) {
                if (ancestor == ENT_PID_NU_BAR_E) return 1.;
                return 0.;
        } else if (daughter->pid == ENT_PID_NU_TAU) {
                if (ancestor == ENT_PID_NU_TAU)
                        return 1.;
                else if (ancestor == ENT_PID_TAU)
                        return ancestor_tau(context, daughter);
                return 0.;
        } else if (daughter->pid == ENT_PID_NU_BAR_TAU) {
                if (ancestor == ENT_PID_NU_BAR_TAU)
                        return 1.;
                else if (ancestor == ENT_PID_TAU_BAR)
                        return ancestor_tau(context, daughter);
                return 0.;
        } else if (daughter->pid == ENT_PID_TAU) {
                if (ancestor == ENT_PID_NU_TAU) return 1.;
                return 0.;
        } else if (daughter->pid == ENT_PID_TAU_BAR) {
                if (ancestor == ENT_PID_NU_BAR_TAU) return 1.;
                return 0.;
        } else
                return 0.;
}

/* Polarisation callback for ALOUETTE. A 100% longitudinal polarisation is
 * assumed.
 */
void polarisation_cb(int pid, const double momentum[3], double * polarisation)
{
        double nrm = momentum[0] * momentum[0] + momentum[1] * momentum[1] +
            momentum[2] * momentum[2];
        if (nrm <= 0.) {
                polarisation[0] = 0.;
                polarisation[1] = 0.;
                polarisation[2] = 0.;
                return;
        }
        nrm = 1. / sqrt(nrm);
        polarisation[0] = momentum[0] * nrm;
        polarisation[1] = momentum[1] * nrm;
        polarisation[2] = momentum[2] * nrm;
}

/* Backward transport routine. */
static void transport_backward(struct simulation_context * context,
    struct generic_state * current, long eventid, int generation,
    struct generic_state * final, struct generic_state * tau_at_production)
{
        /* Backup the final state, e.g. the tau at decay. */
        if (generation == 1) memcpy(final, current, sizeof(* final));

        struct generic_state g_state;
        struct ent_state * state = NULL;
        struct pumas_state * tau = NULL;
        double direction[3];
        if (current->is_tau) {
                /* Apply the BMC weight for the tau decay. */
                tau = &current->base.pumas;
                if (context->api.decay || (generation > 1)) {
                        const double Pf =
                            sqrt(tau->kinetic * (tau->kinetic + 2. * tau_mass));
                        tau->weight *= tau_mass / (tau_ctau0 * Pf);
                }

                /* Backward propagate the tau state. */
                memcpy(direction, tau->direction, sizeof(direction));
                const double lambda0 = 3E+07;
                const double p1 = 0.1;
                double x0;
                for (;;) {
                        x0 = tau->grammage;
                        context->pumas->grammage_max =
                            x0 - lambda0 * log(random_uniform01(context));
                        tau->decayed = 0;
                        pumas_transport(context->pumas, tau);
                        if ((!tau->decayed &&
                                (tau->grammage < context->pumas->grammage_max -
                                        FLT_EPSILON)) ||
                            (tau->kinetic + tau_mass >=
                                context->energy_cut - FLT_EPSILON) ||
                            (tau->weight <= 0.))
                                return;
                        if (generation > 1) break;

                        /* Check that the tau is **not** emerging from the
                         * Earth.
                         */
                        const double b = -tau->position[0] * tau->direction[0] -
                            tau->position[1] * tau->direction[1] -
                            tau->position[2] * tau->direction[2];
                        struct generic_state * g = (struct generic_state *)tau;
                        const double d2 =
                            b * b + EARTH_RADIUS * EARTH_RADIUS - g->r * g->r;
                        if ((d2 <= 0.) || (sqrt(d2) > -b)) break;

                        /* Check that the proposed vertex is **not** in air. */
                        if ((g->medium < 10) || (g->density <= 0.)) break;

                        /* If upgoing and in air, randomly recycle the event
                         * by biasing the decay probability at the vertex.
                         * First let us compute the true decay probability.
                         */
                        const double Pf =
                            sqrt(tau->kinetic * (tau->kinetic + 2. * tau_mass));
                        const double lD = tau_ctau0 * Pf / tau_mass;
                        const double lB = lambda0 / g->density;
                        const double pD = lB / (lB + lD);
                        const double pB = lD / (lB + lD); /* For underflow. */
                        if ((pD <= 0.) || (pB <= 0.)) break;

                        /* Draw over the biased probability and re-weight
                         * accordingly.
                         */
                        if (random_uniform01(context) < p1) {
                                tau->weight *= pD / p1;
                                break;
                        } else
                                tau->weight *= pB / (1. - p1);
                }

                /* Backup the tau state at production. */
                if (generation == 1)
                        memcpy(tau_at_production, current,
                            sizeof(*tau_at_production));

                /* Backward generate the production vertex. */
                enum ent_pid pid =
                    (tau->charge < 0.) ? ENT_PID_TAU : ENT_PID_TAU_BAR;
                state = &g_state.base.ent;
                state->pid = pid;
                state->energy = tau->kinetic + tau_mass;
                state->distance = tau->distance;
                state->grammage = tau->grammage;
                state->weight = tau->weight;
                memcpy(state->position, tau->position, sizeof(state->position));
                memcpy(
                    state->direction, tau->direction, sizeof(state->direction));
                g_state.context = context;
                g_state.r = 0.;
                g_state.is_tau = 0;
                g_state.is_inside = -1;
                g_state.has_crossed = -1;
                g_state.cross_count = 0;

                struct ent_medium * medium;
                medium_ent(&context->ent, state, &medium);
                if (medium == NULL) return;
                ent_vertex(physics, &context->ent, state, medium,
                    ENT_PROCESS_NONE, NULL);

                /* Append the effective BMC weight for the transport, in order
                 * to recover a flux convention.
                 */
                double cs;
                ent_physics_cross_section(physics, pid, state->energy,
                    medium->Z, medium->A, ENT_PROCESS_NONE, &cs);
                double density;
                medium->density(medium, state, &density);
                const double lP = 1E-03 * medium->A / (cs * PHYS_NA * density);
                const double Pi =
                    sqrt(tau->kinetic * (tau->kinetic + 2. * tau_mass));
                const double lD = tau_ctau0 * Pi / tau_mass;
                const double lB = lambda0 / density;
                const double p0 = exp(-(tau->grammage - x0) / lambda0);
                state->weight *= lB * lD / ((lB + lD) * lP * p0);

                /* Reset the initial direction if the transverse transport is
                 * disabled. */
                if (context->api.longitudinal)
                        memcpy(state->direction, direction,
                            sizeof(state->direction));
        } else {
                state = &current->base.ent;
                tau = &g_state.base.pumas;
                memcpy(direction, state->direction, sizeof(direction));
        }

        /* Backward propagate the neutrino. */
        enum ent_event event = ENT_EVENT_NONE;
        while ((event != ENT_EVENT_EXIT) &&
            (state->energy < context->energy_cut - FLT_EPSILON)) {
                ent_transport(physics, &context->ent, state, NULL, &event);
                if (state->weight <= 0.) return;
                if (context->api.longitudinal)
                        memcpy(state->direction, direction,
                            sizeof(state->direction));

                if (event == ENT_EVENT_DECAY_TAU) {
                        /* Backward randomise the tau decay. */
                        double momentum[3] = { state->energy *
                                    state->direction[0],
                                state->energy * state->direction[1],
                                state->energy * state->direction[2] };
                        double weight;
                        int trials;
                        for (trials = 0; trials < 20; trials++) {
                                if (alouette_undecay(state->pid, momentum,
                                        &polarisation_cb, DECAY_BIAS,
                                        &weight) == ALOUETTE_RETURN_SUCCESS)
                                        break;
                        }

                        int pid1;
                        if ((alouette_product(&pid1, momentum) !=
                                ALOUETTE_RETURN_SUCCESS) ||
                            (abs(pid1) != ENT_PID_TAU))
                                return;
                        const double p12 = momentum[0] * momentum[0] +
                            momentum[1] * momentum[1] +
                            momentum[2] * momentum[2];
                        const double E1 = sqrt(p12 + tau_mass * tau_mass);
                        if (E1 >= context->energy_cut - FLT_EPSILON) return;

                        /* Update the tau state and start again the BMC
                         * transport.
                         */
                        tau->charge = (pid1 > 0) ? -1. : 1.;
                        tau->kinetic = E1 - tau_mass;
                        tau->distance = state->distance;
                        tau->grammage = state->grammage;
                        tau->time = 0.;
                        tau->weight = state->weight * weight * state->energy *
                            state->energy / p12;
                        memcpy(tau->position, state->position,
                            sizeof(tau->position));
                        struct generic_state * g = (struct generic_state *)tau;
                        if (context->api.longitudinal) {
                                if (g != current)
                                        memcpy(tau->direction, direction,
                                            sizeof(tau->direction));
                        } else {
                                const double d = 1. / sqrt(p12);
                                tau->direction[0] = momentum[0] * d;
                                tau->direction[1] = momentum[1] * d;
                                tau->direction[2] = momentum[2] * d;
                        }
                        tau->decayed = 0;
                        g->r = 0.;
                        g->is_tau = 1;
                        g->is_inside = -1;
                        g->has_crossed = -1;
                        g->cross_count = 0;
                        generation++;
                        transport_backward(context, g, eventid, generation,
                            final, tau_at_production);
                        return;
                }
        }
        if (event != ENT_EVENT_EXIT) return;
        enum ent_pid pid0;
        if (final->is_tau)
                pid0 = (final->base.pumas.charge < 0.) ? ENT_PID_NU_TAU :
                                                         ENT_PID_NU_BAR_TAU;
        else
                pid0 = final->base.ent.pid;
        if (state->pid != pid0) return;

        /* This is a valid event. In flux mode let us dump the tau state and
         * then return.
         */
        if (!context->api.decay) {
                /* Log the primary neutrino state. */
                format_ancestor(context, eventid, state);

                if (context->flux_neutrino) {
                        /* Log the neutrino final state. */
                        format_neutrino(context, generation, & final->base.ent);
                } else {
                        /* Log the end points of the tau state. */
                        enum ent_pid pid = (final->base.pumas.charge < 0.) ?
                            ENT_PID_TAU :
                            ENT_PID_TAU_BAR;
                        format_tau(context, generation, pid,
                            &tau_at_production->base.pumas, & final->base
                                                                .pumas);
                }
                return;
        }

        /* In full mode let us perform the tau decay with ALOUETTE/TAUOLA. */
        enum ent_pid pid =
            (final->base.pumas.charge < 0.) ? ENT_PID_TAU : ENT_PID_TAU_BAR;
        const double p = sqrt(final->base.pumas.kinetic *
            (final->base.pumas.kinetic + 2. * tau_mass));
        double momentum[3] = { p * final->base.pumas.direction[0],
                p * final->base.pumas.direction[1],
                p * final->base.pumas.direction[2] };
        int trials;
        for (trials = 0; trials < 20; trials++) {
                if (alouette_decay(pid, momentum,
                        final->base.pumas.direction) == ALOUETTE_RETURN_SUCCESS)
                        break;
        }

        int pid1, nprod = 0;
        while (alouette_product(&pid1, momentum) == ALOUETTE_RETURN_SUCCESS) {
                if (abs(pid1 == 12) || (abs(pid1) == 13) || (abs(pid1) == 14) ||
                    (abs(pid1) == 16))
                        continue;
                if (nprod == 0) {
                        /* Log the primary neutrino state. */
                        format_ancestor(context, eventid, state);

                        /* Log the end points of the tau state. */
                        format_tau(context, generation, pid,
                            &tau_at_production->base.pumas, & final->base
                                                                .pumas);
                }
                format_decay_product(context, pid1, momentum);
                nprod++;
        }
}

/* Shortcut for dumping a PUMAS error. */
#define ERROR_PUMAS(rc, function)                                              \
        fprintf(stderr, "error : ");                                           \
        pumas_error_print(stderr, rc, (pumas_function_t *)&function, NULL);    \
        fprintf(stderr, "\n")

/* Loader for PUMAS. */
static int load_pumas(void)
{
        const enum pumas_particle particle = PUMAS_PARTICLE_TAU;
        const char * dump = "materials.b";

        /* First, attempt to load any binary dump. */
        FILE * stream = fopen(dump, "rb");
        if (stream != NULL) {
                enum pumas_return rc;
                if ((rc = pumas_load(stream)) != PUMAS_RETURN_SUCCESS) {
                        fclose(stream);
                        ERROR_PUMAS(rc, pumas_load);
                        return EXIT_FAILURE;
                }
                fclose(stream);
                pumas_particle(NULL, &tau_ctau0, &tau_mass);
                return EXIT_SUCCESS;
        }

        /* If no binary dump, initialise from the MDF and dump. */
        pumas_initialise(particle, NULL, NULL, NULL);
        pumas_particle(NULL, &tau_ctau0, &tau_mass);

        /* Dump the library configuration. */
        stream = fopen(dump, "wb+");
        if (stream == NULL) return EXIT_FAILURE;
        pumas_dump(stream);
        fclose(stream);

        return EXIT_SUCCESS;
}

/* Initialise the DANTON library. */
int danton_initialise(
    const char * pdf, danton_lock_cb * lock, danton_lock_cb * unlock)
{
        /* Clear the error status. */
        errno = 0;

        /* Create a new neutrino Physics environment. */
        ent_physics_create(&physics, pdf);

        /* Initialise the PUMAS transport engine. */
        if (load_pumas() != EXIT_SUCCESS) return EXIT_FAILURE;

        /* Initialise ALOUETTE/TAUOLA. */
        enum alouette_return a_rc;
        if ((a_rc = alouette_initialise(1, NULL)) != ALOUETTE_RETURN_SUCCESS) {
                fprintf(stderr, "alouette_initialise: %s\n",
                    alouette_strerror(a_rc));
                return EXIT_FAILURE;
        };

        return EXIT_SUCCESS;
}

/* Finalise the DANTON library. */
void danton_finalise(void)
{
        ent_physics_destroy(&physics);
        pumas_finalise();
        alouette_finalise();
}

/* Replace the sea layer of the PEM with standard rock. */
void danton_pem_dry(void)
{
        memcpy(media_ent + 9, media_ent + 8, sizeof(*media_ent));
        memcpy(media_pumas + 9, media_pumas + 8, sizeof(*media_pumas));
}

/* Low level data structure for an event sampler. */
struct event_sampler {
        struct danton_sampler api;
        char endstr;
        double neutrino_weight;
        double total_weight;
        unsigned long hash;
};

/* Create a new event sampler. */
struct danton_sampler * danton_sampler_create(void)
{
        struct event_sampler * sampler;
        sampler = malloc(sizeof(*sampler));
        if (sampler == NULL) {
                fprintf(stderr, "danton.c (%d): couldn't allocate memory.\n",
                    __LINE__);
                return NULL;
        }
        memset(sampler, 0x0, sizeof(*sampler));
        sampler->hash--;
        return &sampler->api;
}

/* Destroy an event sampler. */
void danton_sampler_destroy(struct danton_sampler ** sampler)
{
        free(*sampler);
        *sampler = NULL;
}

/* Bernstein's djb2 hash function, from http://www.cse.yorku.ca/~oz/hash.html.
 */
static unsigned long hash(unsigned char * str)
{
        unsigned long hash = 5381;
        int c;

        while ((c = *str++)) {
                hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
        }
        return hash;
}

/* Update an event sampler. */
int danton_sampler_update(struct danton_sampler * sampler)
{
        struct event_sampler * sampler_ = (struct event_sampler *)sampler;

        /* Check the altitude. */
        if ((sampler->altitude[0] < 0.) ||
            (sampler->altitude[0] > sampler->altitude[1])) {
                fprintf(stderr, "danton: invalid altitude value(s). "
                                "Call with -h, --help for usage.\n");
                return EXIT_FAILURE;
        }

        /* Check cos(theta). TODO: compute it from the elevation data. */
        if ((sampler->cos_theta[0] < 0.) ||
            (sampler->cos_theta[0] > sampler->cos_theta[1]) ||
            (sampler->cos_theta[1] > 1.)) {
                fprintf(stderr, "danton: invalid cos(theta) value(s). "
                                "Call with -h, --help for usage.\n");
                return EXIT_FAILURE;
        }

        /* Check the elevation angle. */
        if ((sampler->elevation[0] < -90.) ||
            (sampler->elevation[0] > sampler->elevation[1]) ||
            (sampler->elevation[1] > 90.)) {
                fprintf(stderr, "danton: invalid elevation value(s). "
                                "Call with -h, --help for usage.\n");
                return EXIT_FAILURE;
        }

        /* Check the energy. */
        if ((sampler->energy[0] < 1E+02) ||
            (sampler->energy[0] > sampler->energy[1]) ||
            (sampler->energy[1] < 1E+12)) {
                fprintf(stderr, "danton: invalid energy values. "
                                "Call with -h, --help for usage.\n");
                return EXIT_FAILURE;
        }

        /* Compute the particle weights. */
        sampler_->neutrino_weight = 0.;
        int i;
        for (i = 0; i < DANTON_PARTICLE_N - 2; i++)
                sampler_->neutrino_weight +=
                    (sampler->weight[i] <= 0.) ? 0. : sampler->weight[i];
        sampler_->total_weight = sampler_->neutrino_weight;
        if (sampler->weight[DANTON_PARTICLE_TAU_BAR] > 0.)
                sampler_->total_weight +=
                    sampler->weight[DANTON_PARTICLE_TAU_BAR];
        if (sampler->weight[DANTON_PARTICLE_TAU] > 0.)
                sampler_->total_weight += sampler->weight[DANTON_PARTICLE_TAU];

        /* Update the hash and return. */
        sampler_->hash = hash((unsigned char *)sampler);
        return EXIT_SUCCESS;
}

/* Create a new simulation context for DANTON. */
struct danton_context * danton_context_create(void)
{
        struct simulation_context * context;
        context = malloc(sizeof(*context));
        if (context == NULL) {
                fprintf(stderr, "danton.c (%d): couldn't allocate memory.\n",
                    __LINE__);
                return NULL;
        }

        /* Initialise the random engine. */
        random_initialise(context);

        /* Initialise the Monte-Carlo contexts. */
        context->ent.medium = &medium_ent;
        context->ent.random = &random_ent;
        context->ent.ancestor = NULL;
        context->ent.distance_max = 0.;
        context->ent.grammage_max = 0.;

        enum pumas_return rc;
        if ((rc = pumas_context_create(0, &context->pumas)) !=
            PUMAS_RETURN_SUCCESS) {
                ERROR_PUMAS(rc, pumas_context_create);
                free(context);
                return NULL;
        }
        context->pumas->medium = &medium_pumas;
        context->pumas->random = &random_pumas;
        context->pumas->user_data = context;

        /* Initialise the public API data. */
        context->api.forward = 0;
        context->api.longitudinal = 0;
        context->api.decay = 1;
        context->api.grammage = 0;
        context->api.sampler = NULL;
        context->api.output = NULL;

        /* The lower (upper) energy bound under (above) which all particles are
         * killed.
         */
        context->energy_cut = -1;

        /* Flag for the dumping of the primary state. */
        context->primary_dumped = 0;

        /* Flag to check if the neutrino flux is requested. */
        context->flux_neutrino = 0;

        return &context->api;
}

/* Destroy a DANTON simulation context. */
void danton_context_destroy(struct danton_context ** context)
{
        if (*context == NULL) return;
        struct simulation_context * context_ =
            (struct simulation_context *)(*context);
        pumas_context_destroy(&context_->pumas);
        free(context_);
        *context = NULL;
}

/* Get the PDG number corresponding to the given table index. */
int danton_particle_pdg(enum danton_particle index)
{
        int pdg[DANTON_PARTICLE_N] = { ENT_PID_NU_BAR_TAU, ENT_PID_NU_BAR_MU,
                ENT_PID_NU_BAR_E, ENT_PID_NU_E, ENT_PID_NU_MU, ENT_PID_NU_TAU,
                ENT_PID_TAU_BAR, ENT_PID_TAU };
        if ((index < 0) || (index >= DANTON_PARTICLE_N)) return 0;
        return pdg[index];
}

/* Get the table index corresponding to the given PDG number. */
enum danton_particle danton_particle_index(int pdg)
{
        if (pdg > 0) {
                if (pdg == ENT_PID_NU_E)
                        return DANTON_PARTICLE_NU_E;
                else if (pdg == ENT_PID_NU_MU)
                        return DANTON_PARTICLE_NU_MU;
                else if (pdg == ENT_PID_NU_TAU)
                        return DANTON_PARTICLE_NU_TAU;
                else if (pdg == ENT_PID_TAU)
                        return DANTON_PARTICLE_TAU;
        } else {
                if (pdg == ENT_PID_NU_BAR_TAU)
                        return DANTON_PARTICLE_NU_BAR_TAU;
                else if (pdg == ENT_PID_NU_BAR_MU)
                        return DANTON_PARTICLE_NU_BAR_MU;
                else if (pdg == ENT_PID_NU_BAR_E)
                        return DANTON_PARTICLE_NU_BAR_E;
                else if (pdg == ENT_PID_TAU_BAR)
                        return DANTON_PARTICLE_TAU_BAR;
        }
        return DANTON_PARTICLE_UNKNOWN;
}

static double sample_linear(struct simulation_context * context,
    const double x[2], long i, long n, double * weight)
{
        double xi;
        if (x[0] < x[1]) {
                const double dx = x[1] - x[0];
                double u;
                if ((context->api.grammage) && (n > 0))
                        u = (n > 1) ? i / (n - 1.) : 0.;
                else {
                        u = random_uniform01(context);
                        if (weight != NULL) *weight *= dx;
                }
                xi = dx * u + x[0];
        } else
                xi = x[0];
        return xi;
}

static double sample_log_or_linear(
    struct simulation_context * context, double x[2], double * weight)
{
        double xi;
        if (x[0] < x[1]) {
                if ((x[0] > 0.) || (x[1] < 0.)) {
                        const double r = log(x[1] / x[0]);
                        xi = x[0] * exp(r * random_uniform01(context));
                        if (weight != NULL) *weight *= fabs(r) * xi;
                } else {
                        const double dx = x[1] - x[0];
                        xi = x[0] + dx * random_uniform01(context);
                        if (weight != NULL) *weight *= dx;
                }
        } else
                xi = x[0];
        return xi;
}

/* Run a DANTON simulation. */
int danton_run(struct danton_context * context, long events)
{
        /* Unpack the various objects. */
        struct simulation_context * context_ =
            (struct simulation_context *)context;
        struct danton_sampler * sampler = context->sampler;
        struct event_sampler * sampler_ = (struct event_sampler *)sampler;

        /* Check and configure the context according to the API. */
        if (sampler == NULL) {
                fprintf(stderr, "danton: no sampler was provided. "
                                "Call with -h, --help for usage.\n");
                return EXIT_FAILURE;
        }

        if (sampler_->hash != hash((unsigned char *)sampler)) {
                fprintf(stderr, "danton: sampler has not been updated. "
                                "Call with -h, --help for usage.\n");
                return EXIT_FAILURE;
        }

        if (context->grammage) {
                if (context->forward) {
                        if (sampler->cos_theta[0] == sampler->cos_theta[1])
                                events = 1;
                        else if (events < 2) {
                                fprintf(stderr,
                                    "danton: numbers of bins must be 2 or "
                                    "more. "
                                    "Call with -h, --help for usage.\n");
                                return EXIT_FAILURE;
                        }
                        context_->flux_neutrino = 1;
                } else {
                        if (sampler->elevation[0] == sampler->elevation[1])
                                events = 1;
                        else if (events < 2) {
                                fprintf(stderr,
                                    "danton: numbers of bins must be 2 or "
                                    "more. "
                                    "Call with -h, --help for usage.\n");
                                return EXIT_FAILURE;
                        }
                }
        } else {
                if (sampler_->total_weight <= 0.) {
                        fprintf(stderr, "danton: no particle to sample. "
                                        "Call with -h, --help for usage.\n");
                        return EXIT_FAILURE;
                }
                if (context->forward)
                        context_->flux_neutrino =
                            sampler_->neutrino_weight > 0.;

                if (context->decay) {
                        if (sampler_->neutrino_weight ==
                            sampler_->total_weight) {
                                fprintf(stderr,
                                    "danton: no tau(s) target to decay. "
                                    "Call with -h, --help for usage.\n");
                                return EXIT_FAILURE;
                        }
                        if (context->forward) {
                                if (sampler_->neutrino_weight > 0.) {
                                        fprintf(stderr, "danton: combining "
                                                        "neutrino(s) and "
                                                        "tau(s) "
                                                        "sampling is not "
                                                        "supported in forward "
                                                        "mode. "
                                                        "Call with -h, --help "
                                                        "for usage.\n");
                                        return EXIT_FAILURE;
                                }

                                if (sampler->altitude[0] ==
                                    sampler->altitude[1]) {
                                        fprintf(stderr,
                                            "danton: no altitude range for "
                                            "tau(s) decays.\n");
                                        return EXIT_FAILURE;
                                }
                        }
                }
        }

        context_->energy_cut = (context->forward) ?
            context->sampler->energy[0] :
            1E+12; /* TODO: set from the primary. */
        context_->pumas->kinetic_limit = context_->energy_cut - tau_mass;

        /* Temporary hack for the projectile. */
        int projectile = ENT_PID_NU_TAU;
        if (!context->grammage) {
                int j;
                for (j = 0; j < DANTON_PARTICLE_N; j++) {
                        if (sampler->weight[j] > 0.) {
                                projectile = danton_particle_pdg(j);
                                break;
                        }
                }
        }

        /* Configure the output stream. */
        FILE * stream = output_create(context_);
        if (stream == NULL) {
                fprintf(stderr, "danton: could not open the output file. "
                                "Call with -h, --help for usage.\n");
                exit(EXIT_FAILURE);
        }

        if (context->grammage)
                print_header_grammage(context->forward, stream);
        else
                print_header_decay(stream);
        output_close(context_, stream);

        /* Run the simulation. */
        if (context->forward) {
                /* Run a bunch of forward Monte-Carlo events. */
                long i;
                for (i = 0; i < events; i++) {
                        const double ct = sample_linear(
                            context_, sampler->cos_theta, i, events, NULL);
                        const double st = sqrt(1. - ct * ct);
                        double weight = 1.;
                        /* TODO: sample from the primary. */
                        const double energy = sample_log_or_linear(
                            context_, sampler->energy, &weight);
                        if (sampler->energy[0] < sampler->energy[1])
                                weight *= sampler->energy[1] *
                                    sampler->energy[0] /
                                    ((sampler->energy[1] - sampler->energy[0]) *
                                              energy * energy);

                        const int crossed = context->decay ? -1 : 0;
                        struct generic_state state = {
                                .base.ent = { projectile, energy, 0., 0.,
                                    weight, { 0., 0., -EARTH_RADIUS - 1E+05 },
                                    { st, 0., ct } },
                                .context = context_,
                                .medium = -1,
                                .density = 0.,
                                .r = 0.,
                                .is_tau = 0,
                                .is_inside = -1,
                                .has_crossed = crossed,
                                .cross_count = 0
                        };
                        struct ent_state ancestor;
                        memcpy(&ancestor, &state.base.ent, sizeof(ancestor));
                        context_->primary_dumped = 0;
                        transport_forward(context_, (struct ent_state *)&state,
                            i, 1, &ancestor);
                        if (context->grammage)
                                format_grammage(
                                    context_, ct, state.base.ent.grammage);
                }
        } else {
                /* Run a bunch of backward Monte-Carlo events. */
                context_->ent.ancestor = &ancestor_cb;
                context_->pumas->forward = 0;

                double cos_theta[2];
                int j;
                for (j = 0; j < 2; j++)
                        cos_theta[j] =
                            cos((90. - sampler->elevation[j]) * M_PI / 180.);

                long i;
                for (i = 0; i < events; i++) {
                        double weight = 1.;
                        const double ct = sample_linear(
                            context_, cos_theta, i, events, &weight);
                        const double st = sqrt(1. - ct * ct);
                        const double energy = sample_log_or_linear(
                            context_, sampler->energy, &weight);
                        const double z0 = sample_log_or_linear(
                            context_, sampler->altitude, &weight);

                        if (!context->grammage && !context_->flux_neutrino) {
                                /* This is a particle Monte-Carlo. */
                                const double charge =
                                    (projectile > 0) ? -1. : 1.;
                                struct generic_state state = {
                                        .base.pumas = { charge,
                                            energy - tau_mass, 0., 0., 0.,
                                            weight,
                                            { 0., 0., EARTH_RADIUS + z0 },
                                            { st, 0., ct }, 0 },
                                        .context = context_,
                                        .medium = -1,
                                        .density = 0.,
                                        .r = 0.,
                                        .is_tau = 1,
                                        .is_inside = -1,
                                        .has_crossed = -1,
                                        .cross_count = 0
                                };
                                struct generic_state tau_at_decay,
                                    tau_at_production;
                                context_->primary_dumped = 0;
                                transport_backward(context_, &state, i, 1,
                                    &tau_at_decay, &tau_at_production);
                        } else if (!context->grammage &&
                            context_->flux_neutrino) {
                                struct generic_state state = {
                                        .base.ent = { projectile, energy, 0.,
                                            0., weight,
                                            { 0., 0., EARTH_RADIUS + z0 },
                                            { st, 0., ct } },
                                        .context = context_,
                                        .medium = -1,
                                        .density = 0.,
                                        .r = 0.,
                                        .is_tau = 0,
                                        .is_inside = -1,
                                        .has_crossed = -1,
                                        .cross_count = 0
                                };
                                struct generic_state daughter;
                                context_->primary_dumped = 0;
                                transport_backward(
                                    context_, &state, i, 1, &daughter, NULL);
                        } else {
                                /* This is a grammage scan. */
                                struct generic_state g_state = {
                                        .base.ent = { projectile, energy, 0.,
                                            0., weight,
                                            { 0., 0., EARTH_RADIUS + z0 },
                                            { st, 0., ct } },
                                        .context = context_,
                                        .medium = -1,
                                        .density = 0.,
                                        .r = 0.,
                                        .is_tau = 0,
                                        .is_inside = -1,
                                        .has_crossed = -1,
                                        .cross_count = 0
                                };

                                struct ent_state * state = &g_state.base.ent;
                                enum ent_event event = ENT_EVENT_NONE;
                                while (event != ENT_EVENT_EXIT) {
                                        ent_transport(NULL, &context_->ent,
                                            state, NULL, &event);
                                }

                                format_grammage(context_, ct, state->grammage);
                        }
                }
        }

        return EXIT_SUCCESS;
}

/*
 * Copyright 1995-2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*
 * NB: these functions have been "upgraded", the deprecated versions (which
 * are compatibility wrappers using these functions) are in rsa_depr.c. -
 * Geoff
 */

/*
 * RSA low level APIs are deprecated for public use, but still ok for
 * internal use.
 */
#include "internal/deprecated.h"

#include <stdio.h>
#include <time.h>
#include "internal/cryptlib.h"
#include <openssl/bn.h>
#include <openssl/self_test.h>
#include "rsa_local.h"

static int rsa_keygen_pairwise_test(RSA *rsa, OSSL_CALLBACK *cb, void *cbarg);
static int rsa_keygen(OPENSSL_CTX *libctx, RSA *rsa, int bits, int primes,
                      BIGNUM *e_value, BN_GENCB *cb, int pairwise_test);

/*
 * NB: this wrapper would normally be placed in rsa_lib.c and the static
 * implementation would probably be in rsa_eay.c. Nonetheless, is kept here
 * so that we don't introduce a new linker dependency. Eg. any application
 * that wasn't previously linking object code related to key-generation won't
 * have to now just because key-generation is part of RSA_METHOD.
 */
int RSA_generate_key_ex(RSA *rsa, int bits, BIGNUM *e_value, BN_GENCB *cb)
{
    if (rsa->meth->rsa_keygen != NULL)
        return rsa->meth->rsa_keygen(rsa, bits, e_value, cb);

    return RSA_generate_multi_prime_key(rsa, bits, RSA_DEFAULT_PRIME_NUM,
                                        e_value, cb);
}

int RSA_generate_multi_prime_key(RSA *rsa, int bits, int primes,
                                 BIGNUM *e_value, BN_GENCB *cb)
{
#ifndef FIPS_MODULE
    /* multi-prime is only supported with the builtin key generation */
    if (rsa->meth->rsa_multi_prime_keygen != NULL) {
        return rsa->meth->rsa_multi_prime_keygen(rsa, bits, primes,
                                                 e_value, cb);
    } else if (rsa->meth->rsa_keygen != NULL) {
        /*
         * However, if rsa->meth implements only rsa_keygen, then we
         * have to honour it in 2-prime case and assume that it wouldn't
         * know what to do with multi-prime key generated by builtin
         * subroutine...
         */
        if (primes == 2)
            return rsa->meth->rsa_keygen(rsa, bits, e_value, cb);
        else
            return 0;
    }
#endif /* FIPS_MODULE */
    return rsa_keygen(NULL, rsa, bits, primes, e_value, cb, 0);
}

static int rsa_keygen(OPENSSL_CTX *libctx, RSA *rsa, int bits, int primes,
                      BIGNUM *e_value, BN_GENCB *cb, int pairwise_test)
{
    int ok = -1;
#ifdef FIPS_MODULE
    if (primes != 2)
        return 0;
    ok = rsa_sp800_56b_generate_key(rsa, bits, e_value, cb);
    pairwise_test = 1; /* FIPS MODE needs to always run the pairwise test */
#else
    BIGNUM *r0 = NULL, *r1 = NULL, *r2 = NULL, *tmp, *prime;
    int n = 0, bitsr[RSA_MAX_PRIME_NUM], bitse = 0;
    int i = 0, quo = 0, rmd = 0, adj = 0, retries = 0;
    RSA_PRIME_INFO *pinfo = NULL;
    STACK_OF(RSA_PRIME_INFO) *prime_infos = NULL;
    BN_CTX *ctx = NULL;
    BN_ULONG bitst = 0;
    unsigned long error = 0;

    if (bits < RSA_MIN_MODULUS_BITS) {
        ok = 0;             /* we set our own err */
        RSAerr(0, RSA_R_KEY_SIZE_TOO_SMALL);
        goto err;
    }

    if (primes < RSA_DEFAULT_PRIME_NUM || primes > rsa_multip_cap(bits)) {
        ok = 0;             /* we set our own err */
        RSAerr(0, RSA_R_KEY_PRIME_NUM_INVALID);
        goto err;
    }

    ctx = BN_CTX_new();
    if (ctx == NULL)
        goto err;
    BN_CTX_start(ctx);
    r0 = BN_CTX_get(ctx);
    r1 = BN_CTX_get(ctx);
    r2 = BN_CTX_get(ctx);
    if (r2 == NULL)
        goto err;

    /* divide bits into 'primes' pieces evenly */
    quo = bits / primes;
    rmd = bits % primes;

    for (i = 0; i < primes; i++)
        bitsr[i] = (i < rmd) ? quo + 1 : quo;

    rsa->dirty_cnt++;

    /* We need the RSA components non-NULL */
    if (!rsa->n && ((rsa->n = BN_new()) == NULL))
        goto err;
    if (!rsa->d && ((rsa->d = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->e && ((rsa->e = BN_new()) == NULL))
        goto err;
    if (!rsa->p && ((rsa->p = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->q && ((rsa->q = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->dmp1 && ((rsa->dmp1 = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->dmq1 && ((rsa->dmq1 = BN_secure_new()) == NULL))
        goto err;
    if (!rsa->iqmp && ((rsa->iqmp = BN_secure_new()) == NULL))
        goto err;

    /* initialize multi-prime components */
    if (primes > RSA_DEFAULT_PRIME_NUM) {
        rsa->version = RSA_ASN1_VERSION_MULTI;
        prime_infos = sk_RSA_PRIME_INFO_new_reserve(NULL, primes - 2);
        if (prime_infos == NULL)
            goto err;
        if (rsa->prime_infos != NULL) {
            /* could this happen? */
            sk_RSA_PRIME_INFO_pop_free(rsa->prime_infos, rsa_multip_info_free);
        }
        rsa->prime_infos = prime_infos;

        /* prime_info from 2 to |primes| -1 */
        for (i = 2; i < primes; i++) {
            pinfo = rsa_multip_info_new();
            if (pinfo == NULL)
                goto err;
            (void)sk_RSA_PRIME_INFO_push(prime_infos, pinfo);
        }
    }

    if (BN_copy(rsa->e, e_value) == NULL)
        goto err;

    /* generate p, q and other primes (if any) */
    for (i = 0; i < primes; i++) {
        adj = 0;
        retries = 0;

        if (i == 0) {
            prime = rsa->p;
        } else if (i == 1) {
            prime = rsa->q;
        } else {
            pinfo = sk_RSA_PRIME_INFO_value(prime_infos, i - 2);
            prime = pinfo->r;
        }
        BN_set_flags(prime, BN_FLG_CONSTTIME);

        for (;;) {
 redo:
            if (!BN_generate_prime_ex(prime, bitsr[i] + adj, 0, NULL, NULL, cb))
                goto err;
            /*
             * prime should not be equal to p, q, r_3...
             * (those primes prior to this one)
             */
            {
                int j;

                for (j = 0; j < i; j++) {
                    BIGNUM *prev_prime;

                    if (j == 0)
                        prev_prime = rsa->p;
                    else if (j == 1)
                        prev_prime = rsa->q;
                    else
                        prev_prime = sk_RSA_PRIME_INFO_value(prime_infos,
                                                             j - 2)->r;

                    if (!BN_cmp(prime, prev_prime)) {
                        goto redo;
                    }
                }
            }
            if (!BN_sub(r2, prime, BN_value_one()))
                goto err;
            ERR_set_mark();
            BN_set_flags(r2, BN_FLG_CONSTTIME);
            if (BN_mod_inverse(r1, r2, rsa->e, ctx) != NULL) {
               /* GCD == 1 since inverse exists */
                break;
            }
            error = ERR_peek_last_error();
            if (ERR_GET_LIB(error) == ERR_LIB_BN
                && ERR_GET_REASON(error) == BN_R_NO_INVERSE) {
                /* GCD != 1 */
                ERR_pop_to_mark();
            } else {
                goto err;
            }
            if (!BN_GENCB_call(cb, 2, n++))
                goto err;
        }

        bitse += bitsr[i];

        /* calculate n immediately to see if it's sufficient */
        if (i == 1) {
            /* we get at least 2 primes */
            if (!BN_mul(r1, rsa->p, rsa->q, ctx))
                goto err;
        } else if (i != 0) {
            /* modulus n = p * q * r_3 * r_4 ... */
            if (!BN_mul(r1, rsa->n, prime, ctx))
                goto err;
        } else {
            /* i == 0, do nothing */
            if (!BN_GENCB_call(cb, 3, i))
                goto err;
            continue;
        }
        /*
         * if |r1|, product of factors so far, is not as long as expected
         * (by checking the first 4 bits are less than 0x9 or greater than
         * 0xF). If so, re-generate the last prime.
         *
         * NOTE: This actually can't happen in two-prime case, because of
         * the way factors are generated.
         *
         * Besides, another consideration is, for multi-prime case, even the
         * length modulus is as long as expected, the modulus could start at
         * 0x8, which could be utilized to distinguish a multi-prime private
         * key by using the modulus in a certificate. This is also covered
         * by checking the length should not be less than 0x9.
         */
        if (!BN_rshift(r2, r1, bitse - 4))
            goto err;
        bitst = BN_get_word(r2);

        if (bitst < 0x9 || bitst > 0xF) {
            /*
             * For keys with more than 4 primes, we attempt longer factor to
             * meet length requirement.
             *
             * Otherwise, we just re-generate the prime with the same length.
             *
             * This strategy has the following goals:
             *
             * 1. 1024-bit factors are efficient when using 3072 and 4096-bit key
             * 2. stay the same logic with normal 2-prime key
             */
            bitse -= bitsr[i];
            if (!BN_GENCB_call(cb, 2, n++))
                goto err;
            if (primes > 4) {
                if (bitst < 0x9)
                    adj++;
                else
                    adj--;
            } else if (retries == 4) {
                /*
                 * re-generate all primes from scratch, mainly used
                 * in 4 prime case to avoid long loop. Max retry times
                 * is set to 4.
                 */
                i = -1;
                bitse = 0;
                continue;
            }
            retries++;
            goto redo;
        }
        /* save product of primes for further use, for multi-prime only */
        if (i > 1 && BN_copy(pinfo->pp, rsa->n) == NULL)
            goto err;
        if (BN_copy(rsa->n, r1) == NULL)
            goto err;
        if (!BN_GENCB_call(cb, 3, i))
            goto err;
    }

    if (BN_cmp(rsa->p, rsa->q) < 0) {
        tmp = rsa->p;
        rsa->p = rsa->q;
        rsa->q = tmp;
    }

    /* calculate d */

    /* p - 1 */
    if (!BN_sub(r1, rsa->p, BN_value_one()))
        goto err;
    /* q - 1 */
    if (!BN_sub(r2, rsa->q, BN_value_one()))
        goto err;
    /* (p - 1)(q - 1) */
    if (!BN_mul(r0, r1, r2, ctx))
        goto err;
    /* multi-prime */
    for (i = 2; i < primes; i++) {
        pinfo = sk_RSA_PRIME_INFO_value(prime_infos, i - 2);
        /* save r_i - 1 to pinfo->d temporarily */
        if (!BN_sub(pinfo->d, pinfo->r, BN_value_one()))
            goto err;
        if (!BN_mul(r0, r0, pinfo->d, ctx))
            goto err;
    }

    {
        BIGNUM *pr0 = BN_new();

        if (pr0 == NULL)
            goto err;

        BN_with_flags(pr0, r0, BN_FLG_CONSTTIME);
        if (!BN_mod_inverse(rsa->d, rsa->e, pr0, ctx)) {
            BN_free(pr0);
            goto err;               /* d */
        }
        /* We MUST free pr0 before any further use of r0 */
        BN_free(pr0);
    }

    {
        BIGNUM *d = BN_new();

        if (d == NULL)
            goto err;

        BN_with_flags(d, rsa->d, BN_FLG_CONSTTIME);

        /* calculate d mod (p-1) and d mod (q - 1) */
        if (!BN_mod(rsa->dmp1, d, r1, ctx)
            || !BN_mod(rsa->dmq1, d, r2, ctx)) {
            BN_free(d);
            goto err;
        }

        /* calculate CRT exponents */
        for (i = 2; i < primes; i++) {
            pinfo = sk_RSA_PRIME_INFO_value(prime_infos, i - 2);
            /* pinfo->d == r_i - 1 */
            if (!BN_mod(pinfo->d, d, pinfo->d, ctx)) {
                BN_free(d);
                goto err;
            }
        }

        /* We MUST free d before any further use of rsa->d */
        BN_free(d);
    }

    {
        BIGNUM *p = BN_new();

        if (p == NULL)
            goto err;
        BN_with_flags(p, rsa->p, BN_FLG_CONSTTIME);

        /* calculate inverse of q mod p */
        if (!BN_mod_inverse(rsa->iqmp, rsa->q, p, ctx)) {
            BN_free(p);
            goto err;
        }

        /* calculate CRT coefficient for other primes */
        for (i = 2; i < primes; i++) {
            pinfo = sk_RSA_PRIME_INFO_value(prime_infos, i - 2);
            BN_with_flags(p, pinfo->r, BN_FLG_CONSTTIME);
            if (!BN_mod_inverse(pinfo->t, pinfo->pp, p, ctx)) {
                BN_free(p);
                goto err;
            }
        }

        /* We MUST free p before any further use of rsa->p */
        BN_free(p);
    }

    ok = 1;
 err:
    if (ok == -1) {
        RSAerr(0, ERR_LIB_BN);
        ok = 0;
    }
    BN_CTX_end(ctx);
    BN_CTX_free(ctx);
#endif /* FIPS_MODULE */

    if (pairwise_test && ok > 0) {
        OSSL_CALLBACK *stcb = NULL;
        void *stcbarg = NULL;

        OSSL_SELF_TEST_get_callback(libctx, &stcb, &stcbarg);
        ok = rsa_keygen_pairwise_test(rsa, stcb, stcbarg);
        if (!ok) {
            /* Clear intermediate results */
            BN_clear_free(rsa->d);
            BN_clear_free(rsa->p);
            BN_clear_free(rsa->q);
            BN_clear_free(rsa->dmp1);
            BN_clear_free(rsa->dmq1);
            BN_clear_free(rsa->iqmp);
        }
    }
    return ok;
}

/*
 * For RSA key generation it is not known whether the key pair will be used
 * for key transport or signatures. FIPS 140-2 IG 9.9 states that in this case
 * either a signature verification OR an encryption operation may be used to
 * perform the pairwise consistency check. The simpler encrypt/decrypt operation
 * has been chosen for this case.
 */
static int rsa_keygen_pairwise_test(RSA *rsa, OSSL_CALLBACK *cb, void *cbarg)
{
    int ret = 0;
    unsigned int ciphertxt_len;
    unsigned char *ciphertxt = NULL;
    const unsigned char plaintxt[16] = {0};
    unsigned char decoded[256];
    unsigned int decoded_len;
    unsigned int plaintxt_len = (unsigned int)sizeof(plaintxt_len);
    int padding = RSA_PKCS1_PADDING;
    OSSL_SELF_TEST *st = NULL;

    st = OSSL_SELF_TEST_new(cb, cbarg);
    if (st == NULL)
        goto err;
    OSSL_SELF_TEST_onbegin(st, OSSL_SELF_TEST_TYPE_PCT,
                           OSSL_SELF_TEST_DESC_PCT_RSA_PKCS1);

    ciphertxt_len = RSA_size(rsa);
    ciphertxt = OPENSSL_zalloc(ciphertxt_len);
    if (ciphertxt == NULL)
        goto err;

    ciphertxt_len = RSA_public_encrypt(plaintxt_len, plaintxt, ciphertxt, rsa,
                                       padding);
    if (ciphertxt_len <= 0)
        goto err;
    if (ciphertxt_len == plaintxt_len
        && memcmp(ciphertxt, plaintxt, plaintxt_len) == 0)
        goto err;

    OSSL_SELF_TEST_oncorrupt_byte(st, ciphertxt);

    decoded_len = RSA_private_decrypt(ciphertxt_len, ciphertxt, decoded, rsa,
                                      padding);
    if (decoded_len != plaintxt_len
        || memcmp(decoded, plaintxt,  decoded_len) != 0)
        goto err;

    ret = 1;
err:
    OSSL_SELF_TEST_onend(st, ret);
    OSSL_SELF_TEST_free(st);
    OPENSSL_free(ciphertxt);

    return ret;
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Operand signatures. A signature is the ordered list of fields an instruction's
// semantic helper (ppc::op::<name> in runtime/ppc/ops.h) takes after its fixed
// leading parameters. The same order drives the interpreter, the recompiler's
// emitter, the disassembler and the test encoder.
//
// GEKKO_SIG(name, fields...)

// Branches and system linkage (handled specially by the interpreter and codegen).
GEKKO_SIG(B_I, li, aa, lk)
GEKKO_SIG(B_BC, bo, bi, bd, aa, lk)
GEKKO_SIG(B_BCLR, bo, bi, lk)
GEKKO_SIG(NONE)

// Integer arithmetic / logic.
GEKKO_SIG(D_rD_rA_SIMM, rD, rA, simm)
GEKKO_SIG(D_rA_rS_UIMM, rA, rS, uimm)
GEKKO_SIG(D_CMPI, crfD, rA, simm)
GEKKO_SIG(D_CMPLI, crfD, rA, uimm)
GEKKO_SIG(D_TWI, to, rA, simm)
GEKKO_SIG(XO_rD_rA_rB, rD, rA, rB, oe, rc)
GEKKO_SIG(XO_rD_rA, rD, rA, oe, rc)
GEKKO_SIG(X_rD_rA_rB_RC, rD, rA, rB, rc)
GEKKO_SIG(X_rA_rS_rB, rA, rS, rB, rc)
GEKKO_SIG(X_rA_rS, rA, rS, rc)
GEKKO_SIG(X_SRAWI, rA, rS, sh, rc)
GEKKO_SIG(M_RLWI, rA, rS, sh, mb, me, rc)
GEKKO_SIG(M_RLWNM, rA, rS, rB, mb, me, rc)
GEKKO_SIG(X_CMP, crfD, rA, rB)
GEKKO_SIG(X_TW, to, rA, rB)

// Loads and stores.
GEKKO_SIG(D_LOAD, rD, rA, d)
GEKKO_SIG(D_STORE, rS, rA, d)
GEKKO_SIG(D_FLOAD, frD, rA, d)
GEKKO_SIG(D_FSTORE, frS, rA, d)
GEKKO_SIG(PSQ_D, frD, rA, psq_d, psq_w, psq_i)
GEKKO_SIG(PSQ_X, frD, rA, rB, psqx_w, psqx_i)
GEKKO_SIG(X_LOADX, rD, rA, rB)
GEKKO_SIG(X_STOREX, rS, rA, rB)
GEKKO_SIG(X_FLOADX, frD, rA, rB)
GEKKO_SIG(X_FSTOREX, frS, rA, rB)
GEKKO_SIG(X_LSWI, rD, rA, nb)
GEKKO_SIG(X_STSWI, rS, rA, nb)
GEKKO_SIG(X_CACHE, rA, rB)

// Condition register.
GEKKO_SIG(XL_CRBIT, crbD, crbA, crbB)
GEKKO_SIG(XL_MCRF, crfD, crfS)
GEKKO_SIG(X_MCRXR, crfD)
GEKKO_SIG(X_rD, rD)
GEKKO_SIG(X_MTCRF, crm, rS)

// Special-purpose / supervisor registers.
GEKKO_SIG(X_rS, rS)
GEKKO_SIG(X_MFSPR, rD, spr)
GEKKO_SIG(X_MTSPR, spr, rS)
GEKKO_SIG(X_MFSR, rD, sr)
GEKKO_SIG(X_MTSR, sr, rS)
GEKKO_SIG(X_MFSRIN, rD, rB)
GEKKO_SIG(X_MTSRIN, rS, rB)
GEKKO_SIG(X_TLBIE, rB)

// Floating point and paired singles.
GEKKO_SIG(A_FABC, frD, frA, frC, frB, rc)
GEKKO_SIG(A_FAB, frD, frA, frB, rc)
GEKKO_SIG(A_FAC, frD, frA, frC, rc)
GEKKO_SIG(A_FB, frD, frB, rc)
GEKKO_SIG(X_FB, frD, frB, rc)
GEKKO_SIG(X_FAB, frD, frA, frB, rc)
GEKKO_SIG(X_FCMP, crfD, frA, frB)
GEKKO_SIG(X_MFFS, frD, rc)
GEKKO_SIG(X_MTFSB, crbD, rc)
GEKKO_SIG(X_MTFSFI, crfD, imm4, rc)
GEKKO_SIG(X_MTFSF, fm, frB, rc)
GEKKO_SIG(X_MCRFS, crfD, crfS)

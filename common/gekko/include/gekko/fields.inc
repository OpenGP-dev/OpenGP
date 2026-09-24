// SPDX-License-Identifier: GPL-3.0-or-later
//
// Instruction fields of the Gekko ISA.
//
// GEKKO_FIELD(name, shift, width, signedness, kind)
//   shift       bit position of the field's LSB, counting from the LSB of the 32-bit word
//   width       field width in bits
//   signedness  U (unsigned) or S (sign-extended)
//   kind        how the value is printed; see gekko::FieldKind
//
// A few fields do not fit this pattern and are defined by hand in fields.h:
//   spr  (its two 5-bit halves are swapped)
//   li, bd  (branch displacements, scaled by 4)

GEKKO_FIELD(rD, 21, 5, U, Gpr)
GEKKO_FIELD(rS, 21, 5, U, Gpr)
GEKKO_FIELD(rA, 16, 5, U, Gpr)
GEKKO_FIELD(rB, 11, 5, U, Gpr)
GEKKO_FIELD(frD, 21, 5, U, Fpr)
GEKKO_FIELD(frS, 21, 5, U, Fpr)
GEKKO_FIELD(frA, 16, 5, U, Fpr)
GEKKO_FIELD(frB, 11, 5, U, Fpr)
GEKKO_FIELD(frC, 6, 5, U, Fpr)
GEKKO_FIELD(crbD, 21, 5, U, Crb)
GEKKO_FIELD(crbA, 16, 5, U, Crb)
GEKKO_FIELD(crbB, 11, 5, U, Crb)
GEKKO_FIELD(crfD, 23, 3, U, Crf)
GEKKO_FIELD(crfS, 18, 3, U, Crf)
GEKKO_FIELD(to, 21, 5, U, Dec)
GEKKO_FIELD(bo, 21, 5, U, Dec)
GEKKO_FIELD(bi, 16, 5, U, Dec)
GEKKO_FIELD(sh, 11, 5, U, Dec)
GEKKO_FIELD(nb, 11, 5, U, Dec)
GEKKO_FIELD(mb, 6, 5, U, Dec)
GEKKO_FIELD(me, 1, 5, U, Dec)
GEKKO_FIELD(simm, 0, 16, S, SImm)
GEKKO_FIELD(uimm, 0, 16, U, UImm)
GEKKO_FIELD(d, 0, 16, S, Disp)
GEKKO_FIELD(psq_d, 0, 12, S, Disp)
GEKKO_FIELD(psq_w, 15, 1, U, Dec)
GEKKO_FIELD(psq_i, 12, 3, U, Dec)
GEKKO_FIELD(psqx_w, 10, 1, U, Dec)
GEKKO_FIELD(psqx_i, 7, 3, U, Dec)
GEKKO_FIELD(crm, 12, 8, U, Hex)
GEKKO_FIELD(fm, 17, 8, U, Hex)
GEKKO_FIELD(imm4, 12, 4, U, Dec)
GEKKO_FIELD(sr, 16, 4, U, Dec)
GEKKO_FIELD(aa, 1, 1, U, Flag)
GEKKO_FIELD(lk, 0, 1, U, Flag)
GEKKO_FIELD(rc, 0, 1, U, Flag)
GEKKO_FIELD(oe, 10, 1, U, Flag)

/* SPDX-License-Identifier: GPL-2.0-only */
/* Beam NTC tables recovered from the device Android 4.19.219 kernel.
 * Resistance in ohms, index 0 = -40 C, index 165 = 125 C.
 * ADC and integer interpolation match the device for valid samples;
 * open/short and legacy fabricated fallback temperatures are rejected.
 */
#ifndef BEAM_NTC_CONVERSION_H
#define BEAM_NTC_CONVERSION_H
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdint.h>
typedef uint32_t u32;
typedef int64_t s64;
#endif
static const u32 beam_ntc_board[166] = {
	188020, 177804, 168214, 159206, 150743, 142787, 135305, 128265, 121639,
	115400, 109522, 103989, 98772,	93851,	89207,	84822,	80681,	76769,
	73072,	69576,	66269,	63147,	60192,	57393,	54741,	52228,	49845,
	47585,	45442,	43407,	41476,	39634,	37885,	36224,	34646,	33146,
	31720,	30364,	29074,	27846,	26678,	25569,	24512,	23505,	22545,
	21629,	20756,	19922,	19127,	18368,	17643,	16949,	16287,	15654,
	15049,	14471,	13918,	13389,	12884,	12400,	11937,	11494,	11070,
	10663,	10273,	9900,	9534,	9183,	8847,	8525,	8216,	7920,
	7636,	7364,	7103,	6853,	6613,	6382,	6161,	5948,	5744,
	5547,	5358,	5176,	5001,	4833,	4671,	4515,	4366,	4222,
	4083,	3949,	3821,	3697,	3578,	3463,	3352,	3246,	3143,
	3044,	2948,	2857,	2770,	2685,	2604,	2525,	2450,	2377,
	2306,	2238,	2172,	2108,	2046,	1987,	1929,	1874,	1820,
	1768,	1718,	1669,	1622,	1577,	1533,	1490,	1449,	1410,
	1371,	1334,	1298,	1263,	1229,	1196,	1164,	1133,	1103,
	1074,	1046,	1019,	992,	966,	941,	917,	894,	872,
	850,	828,	808,	788,	768,	749,	731,	713,	696,
	679,	663,	647,	631,	616,	602,	587,	574,	560,
	547,	534,	522,	510,
};
static const u32 beam_ntc_battery[166] = {
	203378, 192285, 181862, 172064, 162851, 154185, 146030, 138354, 131126,
	124317, 117901, 111854, 106151, 100772, 95697,	90907,	86383,	82111,
	78075,	74260,	70654,	67243,	64017,	60964,	58073,	55337,	52745,
	50289,	47961,	45754,	43662,	41676,	39793,	38005,	36308,	34696,
	33164,	31709,	30326,	29011,	27760,	26570,	25438,	24361,	23335,
	22358,	21427,	20540,	19695,	18890,	18121,	17389,	16690,	16022,
	15386,	14778,	14197,	13642,	13113,	12606,	12122,	11659,	11216,
	10793,	10388,	10000,	9629,	9273,	8933,	8607,	8295,	7995,
	7708,	7433,	7169,	6916,	6673,	6440,	6217,	6002,	5796,
	5598,	5408,	5225,	5050,	4881,	4719,	4563,	4413,	4269,
	4130,	3996,	3868,	3744,	3625,	3510,	3400,	3293,	3191,
	3092,	2997,	2905,	2816,	2731,	2648,	2569,	2492,	2418,
	2347,	2278,	2211,	2147,	2085,	2025,	1967,	1911,	1857,
	1804,	1754,	1705,	1657,	1612,	1567,	1524,	1483,	1443,
	1404,	1366,	1330,	1295,	1261,	1227,	1195,	1164,	1134,
	1105,	1077,	1049,	1023,	997,	972,	948,	924,	901,
	879,	858,	837,	816,	797,	777,	759,	741,	723,
	706,	690,	674,	658,	643,	628,	614,	600,	586,
	573,	560,	547,	535,
};

static inline int beam_ntc_convert(int raw, int battery, int *mc)
{
	const u32 *table = battery ? beam_ntc_battery : beam_ntc_board;
	int mv, i, deci;
	u32 ohm;
	if (raw <= 0 || raw >= 1023)
		return -ERANGE;
	mv = raw * 1800 / 1023;
	if (mv <= 0 || mv >= 1800)
		return -ERANGE;
	ohm = mv * 10000 / (1800 - mv);
	if (ohm < table[165] || ohm >= table[0] - 1)
		return -ERANGE;
	for (i = 1; i < 166; i++) {
		if (ohm <= table[i])
			continue;
		if (battery) {
			*mc = (i - 40) * 1000;
			return 0;
		}
		/* The Android first interval uses an unrelated literal 666. */
		if (i <= 1)
			return -ERANGE;
		deci = (i - 40) * 10 +
		       (ohm - table[i]) * 10 / (table[i - 1] - table[i]);
		if (deci < -400 || deci > 1250)
			return -ERANGE;
		*mc = deci * 100;
		return 0;
	}
	return -ERANGE;
}

static inline int beam_ntc_outside(const int *filtered)
{
	s64 value = 112826389LL * filtered[1] - 21524803LL * filtered[0] -
		    4922273LL * filtered[2] + 4288070800LL;
	/* Round to 0.1 C; do not use ambient compensation below its valid range. */
	if (filtered[0] < 23100 || filtered[0] >= 120000)
		return filtered[0];
	return ((value + (value >= 0 ? 5000000000LL : -5000000000LL)) /
		10000000000LL) *
	       100;
}
#endif

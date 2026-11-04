/*
 * KickEQ - stereo parametric EQ (VST3 + CLAP)
 * Copyright (C) 2026 Safian Szabolcs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "kickeq/params.h"

#include <cmath>
#include <cstring>

namespace kickeq
{
namespace
{
// Text conversion is locale independent on purpose (no snprintf/strtod): hosts may run with a locale whose
// decimal separator is a comma.

struct TextWriter
{
    char* out;
    size_t cap;
    size_t len = 0; // length the text would have

    void put (char c) noexcept
    {
        if (cap > 0 && len + 1 < cap)
            out[len] = c;
        ++len;
    }

    void put (const char* s) noexcept
    {
        while (*s != '\0')
            put (*s++);
    }

    size_t finish() noexcept
    {
        if (cap == 0)
            return 0;
        const size_t written = len + 1 < cap ? len : cap - 1;
        out[written] = '\0';
        return written;
    }
};

/** Fixed point formatting with `decimals` digits; optionally strips trailing zeros ("2.50" -> "2.5"). */
void putNumber (TextWriter& w, double v, int decimals, bool trim) noexcept
{
    double scale = 1.0;
    for (int i = 0; i < decimals; ++i)
        scale *= 10.0;

    const bool negative = v < 0.0;
    const double mag = std::fabs (v);
    const double scaledD = std::floor (mag * scale + 0.5);
    // values are bounded by the parameter ranges (<= 30000 * 1000), well inside uint64
    const uint64_t scaled = scaledD < 1.0e15 ? static_cast<uint64_t> (scaledD) : 0;
    const uint64_t div = static_cast<uint64_t> (scale);
    const uint64_t ip = scaled / div;
    uint64_t fp = scaled % div;

    if (negative && scaled != 0)
        w.put ('-');

    char digits[24];
    int n = 0;
    uint64_t t = ip;
    do
    {
        digits[n++] = static_cast<char> ('0' + static_cast<int> (t % 10));
        t /= 10;
    } while (t != 0 && n < 23);
    while (n > 0)
        w.put (digits[--n]);

    int fracDigits = decimals;
    if (trim)
        while (fracDigits > 0 && fp % 10 == 0)
        {
            fp /= 10;
            --fracDigits;
        }

    if (fracDigits > 0)
    {
        w.put ('.');
        char frac[24];
        for (int i = fracDigits - 1; i >= 0; --i)
        {
            frac[i] = static_cast<char> ('0' + static_cast<int> (fp % 10));
            fp /= 10;
        }
        for (int i = 0; i < fracDigits; ++i)
            w.put (frac[i]);
    }
}

char lower (char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char> (c - 'A' + 'a') : c; }
bool isSpace (char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isDigit (char c) noexcept { return c >= '0' && c <= '9'; }

const char* skipSpaces (const char* s) noexcept
{
    while (isSpace (*s))
        ++s;
    return s;
}

/** Parses [+-] digits [.,] digits. Returns false when there is no number at all. */
bool parseNumber (const char*& s, double& value) noexcept
{
    const char* p = skipSpaces (s);
    bool negative = false;
    if (*p == '+' || *p == '-')
    {
        negative = (*p == '-');
        ++p;
    }

    bool any = false;
    double v = 0.0;
    while (isDigit (*p))
    {
        v = v * 10.0 + (*p - '0');
        any = true;
        ++p;
    }
    if (*p == '.' || *p == ',')
    {
        ++p;
        double scale = 0.1;
        while (isDigit (*p))
        {
            v += (*p - '0') * scale;
            scale *= 0.1;
            any = true;
            ++p;
        }
    }

    if (!any)
        return false;
    value = negative ? -v : v;
    s = p;
    return true;
}

/** Case-insensitive comparison ignoring spaces; used for enum labels and suffixes. */
bool equalsLoose (const char* a, const char* b) noexcept
{
    for (;;)
    {
        while (*a == ' ')
            ++a;
        while (*b == ' ')
            ++b;
        if (lower (*a) != lower (*b))
            return false;
        if (*a == '\0')
            return true;
        ++a;
        ++b;
    }
}

bool endsLoose (const char* s) noexcept { return *skipSpaces (s) == '\0'; }

bool hasUnit (const ParamInfo& info, const char* unit) noexcept { return std::strcmp (info.unit, unit) == 0; }
} // namespace

float toNormalized (const ParamInfo& info, float plain) noexcept
{
    plain = clampParam (info, plain);

    if (info.kind != Kind::Float)
    {
        const float span = info.max - info.min;
        return span > 0.0f ? (plain - info.min) / span : 0.0f;
    }

    if (info.scale == Scale::Log && info.min > 0.0f)
        return static_cast<float> (std::log (static_cast<double> (plain) / info.min)
                                   / std::log (static_cast<double> (info.max) / info.min));

    return (plain - info.min) / (info.max - info.min); // Linear and Db
}

float fromNormalized (const ParamInfo& info, float normalized) noexcept
{
    if (!(normalized == normalized))
        return info.def;
    const float n = normalized < 0.0f ? 0.0f : (normalized > 1.0f ? 1.0f : normalized);

    if (info.kind != Kind::Float)
        return clampParam (info, info.min + n * (info.max - info.min));

    if (info.scale == Scale::Log && info.min > 0.0f)
        return clampParam (info, static_cast<float> (info.min * std::pow (static_cast<double> (info.max) / info.min, n)));

    return clampParam (info, info.min + n * (info.max - info.min));
}

size_t valueToText (const ParamInfo& info, float plain, char* out, size_t cap) noexcept
{
    TextWriter w { out, cap };
    const float v = clampParam (info, plain);

    if (info.kind != Kind::Float)
    {
        const int idx = static_cast<int> (v);
        if (info.enumLabels != nullptr && idx >= 0 && idx < info.numEnumLabels)
            w.put (info.enumLabels[idx]);
        else
            putNumber (w, v, 0, false);
    }
    else if (hasUnit (info, "Hz"))
    {
        if (v >= 1000.0f)
        {
            putNumber (w, v / 1000.0, 2, true);
            w.put (" kHz");
        }
        else
        {
            putNumber (w, v, 1, true);
            w.put (" Hz");
        }
    }
    else if (hasUnit (info, "dB"))
    {
        putNumber (w, v, 1, false);
        w.put (" dB");
    }
    else
    {
        putNumber (w, v, 2, false);
        if (info.unit[0] != '\0')
        {
            w.put (' ');
            w.put (info.unit);
        }
    }

    return w.finish();
}

bool textToValue (const ParamInfo& info, const char* text, float& plain) noexcept
{
    if (text == nullptr)
        return false;

    const char* s = skipSpaces (text);

    if (info.kind == Kind::Bool)
    {
        static constexpr const char* yes[] = { "on", "true", "yes", "1" };
        static constexpr const char* no[] = { "off", "false", "no", "0" };
        for (const char* y : yes)
            if (equalsLoose (s, y))
            {
                plain = 1.0f;
                return true;
            }
        for (const char* n : no)
            if (equalsLoose (s, n))
            {
                plain = 0.0f;
                return true;
            }
        return false;
    }

    if (info.kind == Kind::Enum)
    {
        for (int i = 0; i < info.numEnumLabels; ++i)
            if (equalsLoose (s, info.enumLabels[i]))
            {
                plain = static_cast<float> (i);
                return true;
            }

        double number = 0.0;
        const char* p = s;
        if (!parseNumber (p, number))
            return false;

        // numeric labels ("24 dB/oct"): compare the leading number; accept a trailing "dB"/"dB/oct" suffix
        const bool numericLabels = info.numEnumLabels > 0 && isDigit (info.enumLabels[0][0]);
        if (numericLabels)
        {
            const char* suffix = skipSpaces (p);
            if (!(*suffix == '\0' || equalsLoose (suffix, "db") || equalsLoose (suffix, "db/oct")))
                return false;

            for (int i = 0; i < info.numEnumLabels; ++i)
            {
                const char* l = info.enumLabels[i];
                double labelNumber = 0.0;
                if (parseNumber (l, labelNumber) && labelNumber == number)
                {
                    plain = static_cast<float> (i);
                    return true;
                }
            }
            return false;
        }

        if (!endsLoose (p) || number < 0.0 || number != std::floor (number) || number > info.numEnumLabels - 1)
            return false;
        plain = static_cast<float> (static_cast<int> (number));
        return true;
    }

    double number = 0.0;
    const char* p = s;
    if (!parseNumber (p, number))
        return false;
    p = skipSpaces (p);

    if (hasUnit (info, "Hz"))
    {
        if (equalsLoose (p, "khz") || equalsLoose (p, "k"))
            number *= 1000.0;
        else if (!(equalsLoose (p, "hz") || *p == '\0'))
            return false;
    }
    else if (hasUnit (info, "dB"))
    {
        if (!(equalsLoose (p, "db") || *p == '\0'))
            return false;
    }
    else if (*p != '\0')
    {
        return false;
    }

    if (!std::isfinite (number))
        return false;
    plain = clampParam (info, static_cast<float> (number));
    return true;
}

int indexOfId (uint32_t id) noexcept
{
    if (id < static_cast<uint32_t> (numGlobalParams))
        return static_cast<int> (id);

    if (id >= 0x100u && id < 0x100u + 0x10u * static_cast<uint32_t> (numBands))
    {
        const uint32_t band = (id - 0x100u) >> 4;
        const uint32_t k = id & 0xFu;
        if (k < static_cast<uint32_t> (paramsPerBand))
            return numGlobalParams + static_cast<int> (band) * paramsPerBand + static_cast<int> (k);
    }

    return -1;
}

int indexOfStrId (const char* strId) noexcept
{
    if (strId == nullptr)
        return -1;
    for (int i = 0; i < numParams; ++i)
        if (std::strcmp (paramInfo (i).strId, strId) == 0)
            return i;
    return -1;
}
} // namespace kickeq

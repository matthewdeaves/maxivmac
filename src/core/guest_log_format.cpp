/*
	guest_log_format.cpp — printf-style formatting of guest log messages (see guest_log_format.h)
*/
#include "core/guest_log_format.h"

#include <cstdio>

std::string formatGuestLog(const std::string &fmt, const uint32_t args[7], const GuestStringReader &rd)
{
	std::string out;
	out.reserve(fmt.size() * 2);
	int argIdx = 0;
	char numbuf[20];

	for (size_t i = 0; i < fmt.size(); i++)
	{
		if (fmt[i] != '%')
		{
			out.push_back(fmt[i]);
			continue;
		}
		/* look at next char(s) */
		i++;
		if (i >= fmt.size()) break;

		if (fmt[i] == '%')
		{
			out.push_back('%');
			continue;
		}

		/* consume optional flags/width/precision (e.g. "02", "-8") */
		char fmtspec[16];
		int fpos = 0;
		int digitRun = 0; /* guest-controlled: "%.2000000000x" stalls the host for seconds */
		fmtspec[fpos++] = '%';
		while (i < fmt.size() &&
			   (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '0' || fmt[i] == '#' ||
				(fmt[i] >= '1' && fmt[i] <= '9') || fmt[i] == '.'))
		{
			/* A '0' before any width digit is the zero-pad flag, not part of the
			   number ("%010x" is width 10); a number may have up to 3 digits (<=999). */
			const bool isDigit = fmt[i] >= '0' && fmt[i] <= '9';
			const bool isFlag = fmt[i] == '0' && digitRun == 0;
			if (isDigit && !isFlag) digitRun++;
			else if (!isDigit) digitRun = 0;
			if (fpos < 12 && digitRun <= 3) fmtspec[fpos++] = fmt[i];
			i++;
		}
		if (i >= fmt.size()) break;

		/* consume optional 'l' prefix */
		if (fmt[i] == 'l' && i + 1 < fmt.size()) i++;

		if (argIdx > 5)
		{
			out += "<?>";
			continue;
		}
		uint32_t val = args[argIdx + 1]; /* args[1..6] = p1..p6 */
		argIdx++;

		switch (fmt[i])
		{
			case 'x':
				fmtspec[fpos++] = 'x';
				fmtspec[fpos] = '\0';
				snprintf(numbuf, sizeof(numbuf), fmtspec, val);
				out += numbuf;
				break;
			case 'X':
				fmtspec[fpos++] = 'X';
				fmtspec[fpos] = '\0';
				snprintf(numbuf, sizeof(numbuf), fmtspec, val);
				out += numbuf;
				break;
			case 'd':
				fmtspec[fpos++] = 'd';
				fmtspec[fpos] = '\0';
				snprintf(numbuf, sizeof(numbuf), fmtspec, static_cast<int32_t>(val));
				out += numbuf;
				break;
			case 'u':
				fmtspec[fpos++] = 'u';
				fmtspec[fpos] = '\0';
				snprintf(numbuf, sizeof(numbuf), fmtspec, val);
				out += numbuf;
				break;
			case 's':
				out += rd.cstr(val);
				break;
			case 'S':
				out += rd.pstr(val);
				break;
			default:
				out.push_back('%');
				out.push_back(fmt[i]);
				break;
		}
	}
	return out;
}


#include <sys/types.h>

/**
 * With the switch to C++11 by default, char16_t became a unique type,
 * rather than basically just a typedef of uint16_t. As a result, the
 * compiler now mangles the symbol for writeString16 differently. Our
 * RIL references the old symbol of course, not the new one.
 */
uintptr_t _ZN7android6Parcel13writeString16EPKDsj(void *instance, void *str, size_t len);
uintptr_t _ZN7android6Parcel13writeString16EPKtj(void *instance, void *str, size_t len)
{
	return _ZN7android6Parcel13writeString16EPKDsj(instance, str, len);
}

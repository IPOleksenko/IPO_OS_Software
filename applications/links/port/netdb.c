#include <arpa/inet.h>
#include <netdb.h>
#include <syscall.h>

int h_errno;

static struct in_addr resolved_address;
static char *resolved_addresses[2];
static char *resolved_aliases[1];
static struct hostent resolved_host;

struct hostent *gethostbyname(const char *name)
{
	uint32_t address = 0;
	uint32_t args[3];

	if (!name || !*name) {
		h_errno = HOST_NOT_FOUND;
		return NULL;
	}
	args[0] = (uint32_t)(uintptr_t)name;
	args[1] = (uint32_t)(uintptr_t)&address;
	args[2] = 3000u;
	if (ipo_syscall(IPO_SYSCALL_NET_DNS_RESOLVE, 3u, args) != 1) {
		h_errno = HOST_NOT_FOUND;
		return NULL;
	}

	resolved_address.s_addr = htonl(address);
	resolved_addresses[0] = (char *)&resolved_address;
	resolved_addresses[1] = NULL;
	resolved_aliases[0] = NULL;
	resolved_host.h_name = (char *)name;
	resolved_host.h_aliases = resolved_aliases;
	resolved_host.h_addrtype = AF_INET;
	resolved_host.h_length = sizeof(resolved_address);
	resolved_host.h_addr_list = resolved_addresses;
	h_errno = NETDB_SUCCESS;
	return &resolved_host;
}

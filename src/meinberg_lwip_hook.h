#pragma once

/*
 * ESP-IDF 5.5.5 lwip_default_hooks.h declares
 * lwip_dhcp_on_extra_option(struct dhcp *, ...)
 * before its own `struct dhcp;` forward declaration.
 *
 * This project hook is included earlier by lwip_default_hooks.h, so this
 * declaration makes the type visible first and prevents the C type conflict.
 */
struct dhcp;

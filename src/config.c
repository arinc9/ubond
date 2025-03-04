/*
 * Copyright (c) 2015, Laurent COUSTET <ed@zehome.com>
 *
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS AND CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <ifaddrs.h>

#include "includes.h"
#include "tool.h"
#include "tuntap_generic.h"
#include "ubond.h"

#include "configlib.h"

extern char* status_command;
extern struct ubond_options_s ubond_options;
extern struct ubond_filters_s ubond_filters;
extern struct tuntap_s tuntap;
extern LIST_HEAD(rtunhead, ubond_tunnel_s) rtuns;

char* ip_from_if(char* ifname);
// we'll declair this here, so that any device name used instead of an IP
// address gets translated before we go anywhere else...

/* Config file reading / re-read.
 * config_file_fd: fd opened in priv_open_config
 * first_time: set to 0 for re-read, or 1 for initial configuration
 */
int ubond_config(int config_file_fd, int first_time)
{
    config_t *config, *work;
    ubond_tunnel_t* tmptun;
    char* tmp = NULL;
    char* mode = NULL;
    char* lastSection = NULL;
    char* tundevname = NULL;
    char* password = NULL;
    uint32_t tun_mtu = 0;

    uint32_t default_timeout = 60;
    uint32_t default_server_mode = 0; /* 0 => client */
    //uint32_t cleartext_data = 0;
    uint32_t static_tunnel = 0;
    uint32_t fallback_only = 0;
    uint32_t tcp_socket = 1211;

    ubond_options.fallback_available = 0;

    /* reset all bpf filters on every interface */
#ifdef HAVE_FILTERS
    struct bpf_program filter;
    pcap_t* pcap_dead_p = pcap_open_dead(DLT_RAW, DEFAULT_MTU);
    memset(&ubond_filters, 0, sizeof(ubond_filters));
#endif

    work = config = _conf_parseConfig(config_file_fd);
    if (!config)
        goto error;

    while (work) {
        if ((work->section != NULL) && !mystr_eq(work->section, lastSection)) {
            lastSection = work->section;
            if (mystr_eq(lastSection, "general")) {
                /* Thoses settings can only by set at start time */
                if (first_time) {
                    _conf_set_str_from_conf(
                        config, lastSection, "statuscommand", &status_command, NULL,
                        NULL, 0);
                    _conf_set_str_from_conf(
                        config, lastSection, "interface_name", &tundevname, "ubond0",
                        NULL, 0);
                    if (tundevname) {
                        strlcpy(tuntap.devname, tundevname, sizeof(tuntap.devname));
                        free(tundevname);
                    }
                    _conf_set_str_from_conf(
                        config, lastSection, "tuntap", &tmp, "tun", NULL, 0);
                    if (tmp) {
                        if (mystr_eq(tmp, "tun"))
                            tuntap.type = UBOND_TUNTAPMODE_TUN;
                        else
                            tuntap.type = UBOND_TUNTAPMODE_TAP;
                        free(tmp);
                    }
                    /* Control configuration */
                    _conf_set_str_from_conf(
                        config, lastSection, "control_unix_path", &tmp, NULL,
                        NULL, 0);
                    if (tmp) {
                        strlcpy(ubond_options.control_unix_path, tmp,
                            sizeof(ubond_options.control_unix_path));
                        free(tmp);
                    }
                    _conf_set_str_from_conf(
                        config, lastSection, "control_bind_host", &tmp, NULL,
                        NULL, 0);
                    if (tmp) {
                        strlcpy(ubond_options.control_bind_host, tmp,
                            sizeof(ubond_options.control_bind_host));
                        free(tmp);
                    }
                    _conf_set_str_from_conf(
                        config, lastSection, "control_bind_port", &tmp, NULL,
                        NULL, 0);
                    if (tmp) {
                        strlcpy(ubond_options.control_bind_port, tmp,
                            sizeof(ubond_options.control_bind_port));
                        free(tmp);
                    }
                }
                /* This is important to be parsed every time because
                 * it's used later in the configuration parsing
                 */
                _conf_set_str_from_conf(
                    config, lastSection, "mode", &mode, NULL,
                    "Operation mode is mandatory.", 1);
                if (mystr_eq(mode, "server"))
                    default_server_mode = 1;
                if (mode)
                    free(mode);

                _conf_set_str_from_conf(
                    config, lastSection, "password", &password, NULL,
                    "Password is mandatory.", 2);
                if (password) {
                    log_info("config", "new password set");
                    strlcpy(ubond_options.password, password,
                            sizeof(ubond_options.password));
                    memset(password, 0, strlen(password));
                    free(password);
                }

                _conf_set_uint_from_conf(
                    config, lastSection, "static_tunnel", &static_tunnel, 0,
                    NULL, 0);
                ubond_options.static_tunnel = static_tunnel;

                _conf_set_uint_from_conf(
                    config, lastSection, "timeout", &default_timeout, 60,
                    NULL, 0);
                if (default_timeout < 5) {
                    log_warnx("config", "timeout capped to 5 seconds");
                    default_timeout = 5;
                }

                _conf_set_str_from_conf(
                    config, lastSection, "reorder_buffer", &tmp, NULL, NULL, 0);
                if (tmp) {
                    ubond_reorder_reset();
                    if (strcmp(tmp, "yes") == 0) {
                        ubond_reorder_enable();
                    } else {
                        log_warnx("config", "Reorder buffer disabled");
                    }
                }

                /* Tunnel configuration */
                _conf_set_str_from_conf(
                    config, lastSection, "ip4", &tmp, NULL, NULL, 0);
                if (tmp) {
                    strlcpy(ubond_options.ip4, tmp, sizeof(ubond_options.ip4));
                    free(tmp);
                } else {
                    memset(ubond_options.ip4_gateway, 0,
                        sizeof(ubond_options.ip4_gateway));
                }

                _conf_set_str_from_conf(
                    config, lastSection, "ip6", &tmp, NULL, NULL, 0);
                if (tmp) {
                    strlcpy(ubond_options.ip6, tmp, sizeof(ubond_options.ip6));
                    free(tmp);
                } else {
                    memset(ubond_options.ip4_gateway, 0,
                        sizeof(ubond_options.ip4_gateway));
                }

                _conf_set_str_from_conf(
                    config, lastSection, "ip4_gateway", &tmp, NULL, NULL, 0);
                if (tmp) {
                    strlcpy(ubond_options.ip4_gateway, tmp,
                        sizeof(ubond_options.ip4_gateway));
                    free(tmp);
                } else {
                    memset(ubond_options.ip4_gateway, 0,
                        sizeof(ubond_options.ip4_gateway));
                }

                _conf_set_str_from_conf(
                    config, lastSection, "ip6_gateway", &tmp, NULL, NULL, 0);
                if (tmp) {
                    strlcpy(ubond_options.ip6_gateway, tmp,
                        sizeof(ubond_options.ip6_gateway));
                    free(tmp);
                } else {
                    memset(ubond_options.ip6_gateway, 0,
                        sizeof(ubond_options.ip6_gateway));
                }

                _conf_set_str_from_conf(
                    config, lastSection, "ip4_routes", &tmp, NULL, NULL, 0);
                if (tmp) {
                    strlcpy(ubond_options.ip4_routes, tmp,
                        sizeof(ubond_options.ip4_routes));
                    free(tmp);
                } else {
                    memset(ubond_options.ip4_routes, 0,
                        sizeof(ubond_options.ip4_routes));
                }

                _conf_set_str_from_conf(
                    config, lastSection, "ip6_routes", &tmp, NULL, NULL, 0);
                if (tmp) {
                    strlcpy(ubond_options.ip6_routes, tmp,
                        sizeof(ubond_options.ip6_routes));
                    free(tmp);
                } else {
                    memset(ubond_options.ip6_routes, 0,
                        sizeof(ubond_options.ip6_routes));
                }

                _conf_set_uint_from_conf(
                    config, lastSection, "mtu", &tun_mtu, 1432, NULL, 0);
                if (tun_mtu != 0) {
                    ubond_options.mtu = tun_mtu;
                }
            } else if (strncmp(lastSection, "filters", 7) != 0) {
                char* bindaddr;
                char* bindport;
                char* binddev;
                uint32_t bindfib = 0;
                char* dstaddr;
                char* dstport;
                uint32_t bwlimit = 0;
                uint32_t quota = 0;
                uint32_t timeout = 30;
                int create_tunnel = 1;

                if (default_server_mode) {
                    _conf_set_str_from_conf(
                        config, lastSection, "bindhost", &bindaddr, NULL,
                        NULL, 0);
                    _conf_set_str_from_conf(
                        config, lastSection, "bindport", &bindport, NULL,
                        "bind port is mandatory in server mode.\n", 1);
                    _conf_set_uint_from_conf(
                        config, lastSection, "bindfib", &bindfib, 0,
                        NULL, 0);
                    _conf_set_str_from_conf(
                        config, lastSection, "remotehost", &dstaddr, NULL,
                        NULL, 0);
                    _conf_set_str_from_conf(
                        config, lastSection, "remoteport", &dstport, NULL,
                        NULL, 0);
                } else {
                    _conf_set_str_from_conf(
                        config, lastSection, "bindhost", &bindaddr, NULL,
                        NULL, 0);
                    _conf_set_str_from_conf(
                        config, lastSection, "bindport", &bindport, NULL,
                        NULL, 0);
                    _conf_set_uint_from_conf(
                        config, lastSection, "bindfib", &bindfib, 0,
                        NULL, 0);
                    _conf_set_str_from_conf(
                        config, lastSection, "remotehost", &dstaddr, NULL,
                        "No remote address specified.\n", 1);
                    _conf_set_str_from_conf(
                        config, lastSection, "remoteport", &dstport, NULL,
                        "No remote port specified.\n", 1);
                }

                bindaddr = ip_from_if(bindaddr);

                _conf_set_str_from_conf(
                    config, lastSection, "binddev", &binddev, NULL, NULL, 0);
                _conf_set_uint_from_conf(
                    config, lastSection, "bandwidth_upload", &bwlimit, 0,
                    NULL, 0);
                _conf_set_uint_from_conf(
                    config, lastSection, "quota", &quota, 0,
                    NULL, 0);
                _conf_set_uint_from_conf(
                    config, lastSection, "timeout", &timeout, default_timeout,
                    NULL, 0);
                if (timeout < 5) {
                    log_warnx("config", "timeout capped to 5 seconds");
                    timeout = 5;
                }
                _conf_set_uint_from_conf(
                    config, lastSection, "fallback_only", &fallback_only, 0,
                    NULL, 0);
                if (fallback_only) {
                    ubond_options.fallback_available = 1;
                }
                LIST_FOREACH(tmptun, &rtuns, entries)
                {
                    if (mystr_eq(lastSection, tmptun->name)) {
                        log_info("config",
                            "%s restart for configuration reload",
                            tmptun->name);
                        if ((!mystr_eq(tmptun->bindaddr, bindaddr)) || (!mystr_eq(tmptun->bindport, bindport)) || (tmptun->bindfib != bindfib) || (!mystr_eq(tmptun->destaddr, dstaddr)) || (!mystr_eq(tmptun->destport, dstport)) || (!mystr_eq(tmptun->binddev, binddev))) {
                            ubond_rtun_status_down(tmptun);
                        }

                        if (bindaddr) {
                            strlcpy(tmptun->bindaddr, bindaddr, sizeof(tmptun->bindaddr));
                        }
                        if (bindport) {
                            strlcpy(tmptun->bindport, bindport, sizeof(tmptun->bindport));
                        }
                        if (tmptun->bindfib != bindfib) {
                            tmptun->bindfib = bindfib;
                        }
                        if (binddev) {
                            strlcpy(tmptun->binddev, binddev, sizeof(tmptun->binddev));
                        }
                        if (dstaddr) {
                            strlcpy(tmptun->destaddr, dstaddr, sizeof(tmptun->destaddr));
                        }
                        if (dstport) {
                            strlcpy(tmptun->destport, dstport, sizeof(tmptun->destport));
                        }
                        if (tmptun->fallback_only != fallback_only) {
                            log_info("config", "%s fallback_only changed from %d to %d",
                                tmptun->name, tmptun->fallback_only, fallback_only);
                            tmptun->fallback_only = fallback_only;
                        }
                        if (tmptun->bandwidth_max != bwlimit) {
                            log_info("config", "%s bandwidth changed from %lu to %u",
                                tmptun->name, tmptun->bandwidth_max, bwlimit);
                            if (bwlimit == 0) {
                                bwlimit = 10000;
                            }
                            tmptun->bandwidth_max = bwlimit;
                            //                            tmptun->bandwidth = bwlimit;
                        }
                        if (tmptun->quota != quota) {
                            log_info("config", "%s quota changed from %d to %d",
                                tmptun->name, tmptun->quota, quota);
                            tmptun->quota = quota;
                        }
                        create_tunnel = 0;
                        break; /* Very important ! */
                    }
                }

                if (create_tunnel) {
                    log_info("config", "%s tunnel added", lastSection);
                    ubond_rtun_new(
                        lastSection, bindaddr, bindport, binddev, bindfib, dstaddr, dstport,
                        default_server_mode, timeout, fallback_only,
                        bwlimit, quota);
                }
                if (bindaddr)
                    free(bindaddr);
                if (bindport)
                    free(bindport);
                if (binddev)
                    free(binddev);
                if (dstaddr)
                    free(dstaddr);
                if (dstport)
                    free(dstport);
            }
        } else if (lastSection == NULL)
            lastSection = work->section;
        work = work->next;
    }

    /* Ok, let's delete old tunnels */
    if (!first_time) {
        LIST_FOREACH(tmptun, &rtuns, entries)
        {
            int found_in_config = 0;

            work = config;
            while (work) {
                if (work->conf && work->section && mystr_eq(work->section, tmptun->name)) {
                    found_in_config = 1;
                    break;
                }
                work = work->next;
            }

            if (!found_in_config) {
                log_info("config", "%s tunnel removed", tmptun->name);
                ubond_rtun_drop(tmptun);
            }
        }
    }

#ifdef HAVE_FILTERS
    work = config;
    int found_in_config = 0;
    while (work) {
        if (work->section != NULL && strncmp(work->section, "filters", 7) == 0) {
            memset(&filter, 0, sizeof(filter));
            if (pcap_compile(pcap_dead_p, &filter, work->conf->val,
                    1, PCAP_NETMASK_UNKNOWN)
                != 0) {
                log_warnx("config", "invalid filter %s = %s: %s",
                    work->conf->var, work->conf->val, pcap_geterr(pcap_dead_p));
            } else {
                found_in_config = 0;
                LIST_FOREACH(tmptun, &rtuns, entries)
                {
                    if (strcmp(work->conf->var, tmptun->name) == 0) {
                        if (ubond_filters_add(&filter, tmptun) != 0) {
                            log_warnx("config", "%s filter %s error: too many filters",
                                tmptun->name, work->conf->val);
                        } else {
                            log_debug("config", "%s added filter: %s",
                                tmptun->name, work->conf->val);
                            found_in_config = 1;
                            break;
                        }
                    }
                }
                if (!found_in_config) {
                    log_warnx("config", "(filters) %s interface not found",
                        work->conf->var);
                }
            }
        }
        work = work->next;
    }
#endif

    //_conf_printConfig(config);
    _conf_freeConfig(config);
#ifdef HAVE_FILTERS
    pcap_close(pcap_dead_p);
#endif

    if (first_time && status_command)
        priv_init_script(status_command);
    return 0;
error:
    log_warnx("config", "parse error");
    return 1;
}

/* This is a filter function, it takes an name, if the name turns out to be an
 * interface, it translates it to it's IP address,
 * the resulting filtered name is returned (whether it has matched an interface
 * or not */
char* ip_from_if(char* ifname)
{

    struct ifaddrs *ifaddr, *ifa;
    int s;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1) {
        log_warn(NULL, "unable to collect ifaddrs");
        return ifname;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);

        if ((strcmp(ifa->ifa_name, ifname) == 0) && (ifa->ifa_addr->sa_family == AF_INET)) {
            if (s == 0) {
                if (ifname)
                    free(ifname);
                ifname = strdup(host);
            }
        }
    }

    freeifaddrs(ifaddr);
    return ifname;
}

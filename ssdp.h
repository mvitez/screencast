#ifndef _SSDP_H_INCLUDED_
#define _SSDP_H_INCLUDED_

#ifdef __cplusplus
extern "C"
{
#endif

    int start_upnp_server(int local_port, const char *name);
    char **get_stream_items();
    int serve(int sk, const char *name);

#ifdef __cplusplus
}
#endif

#endif

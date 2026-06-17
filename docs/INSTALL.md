# Installation

See the [README](../README.md) for an overview.

## Requirements

- NGINX (built and tested against 1.30.2) with dynamic module support.
- Redis or Valkey 5.0+ (needs server-side `redis.call('TIME')` for the token
  bucket and GCRA scripts). A stock server is enough — no modules to load.
- The NGINX source tree matching your installed binary, to build the module.

## Build

The module is built against the NGINX source with `--add-dynamic-module`. From
an NGINX source tree matching your installed binary:

```sh
./configure --with-compat --add-dynamic-module=/path/to/nginx-ratelimit
make modules
cp objs/ngx_http_ratelimit_module.so /etc/nginx/modules/
```

Then load it:

```nginx
load_module modules/ngx_http_ratelimit_module.so;
```

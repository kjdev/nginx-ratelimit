# Examples

See the [README](../README.md) for an overview and [DIRECTIVES.md](DIRECTIVES.md)
for the full directive reference.

## Quick start

```nginx
http {
    # A Redis upstream. keepalive pools the connection so the per-request
    # TCP connect (and AUTH/SELECT, if configured) is amortised.
    upstream redis {
        server 127.0.0.1:6379;
        keepalive 32;
    }

    # Define a named rate. No shared memory is allocated; the counter is in Redis.
    ratelimit_zone api key=$binary_remote_addr rate=100r/m;

    server {
        location /api/ {
            ratelimit zone=api;
            ratelimit_pass redis;
            ratelimit_headers on;

            proxy_pass http://backend;
        }
    }
}
```

The 101st request from an address within a minute receives `429 Too Many
Requests`.

## Using an authenticated identity as the key

Delegate authentication to another module and feed its variable in as the key.
For example, with [kjdev/nginx-auth-jwt](https://github.com/kjdev/nginx-auth-jwt)
exposing the `sub` claim:

```nginx
http {
    upstream redis { server redis.internal:6379; keepalive 32; }

    # Per authenticated subject: 1000 requests/hour, smoothed with GCRA.
    ratelimit_zone user key=$jwt_claim_sub requests=1000 period=1h burst=50 algo=gcra;

    server {
        location /api/ {
            auth_jwt          "api";
            auth_jwt_key_file conf/jwt.key;

            ratelimit          zone=user;
            ratelimit_pass     redis;
            ratelimit_password $redis_password;   # e.g. from env via a variable
            ratelimit_headers  on;

            proxy_pass http://backend;
        }
    }
}
```

The exact claim variable name depends on the auth module; `ratelimit` only reads
whatever variable you name in `key=`. If the variable is empty (unauthenticated)
the request is not limited, so pair it with the auth module's own enforcement.

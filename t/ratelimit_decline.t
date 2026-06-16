#!/usr/bin/env perl

# An empty key variable makes the handler return NGX_DECLINED: the request is
# not rate limited at all (no Redis round-trip, no X-RateLimit headers). When
# the key resolves to a non-empty value the limiter runs normally.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};
       keepalive 1024;
    }

    ratelimit_zone byid key=\$arg_id requests=100 period=1m;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: an empty key is declined (request passes, no headers)
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=byid;
        ratelimit_prefix dz;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /t
--- response_headers
!X-RateLimit-Limit
!X-RateLimit-Remaining
!X-RateLimit-Reset
--- response_body
200 OK
--- error_code: 200

=== TEST 2: a non-empty key runs the limiter and emits headers
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=byid;
        ratelimit_prefix dz2;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /t?id=abc
--- response_headers
X-RateLimit-Limit: 100
--- error_code: 200

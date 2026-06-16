#!/usr/bin/env perl

# Burst headroom for the fixed-window algorithm. The effective limit is
# zone "requests" plus "burst". A per-location "ratelimit ... burst=N"
# overrides the zone default. Periods are kept long so the window never rolls
# over mid-test; each block uses a distinct prefix to avoid key collisions.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};
       keepalive 1024;
    }

    # requests=2 + burst=3 => effective limit 5.
    ratelimit_zone burst key=\$remote_addr requests=2 period=1m burst=3;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: zone burst widens the limit to requests + burst
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        ratelimit zone=burst;
        ratelimit_prefix bz;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
['GET /hit', 'GET /hit', 'GET /hit', 'GET /hit', 'GET /hit', 'GET /hit']
--- response_headers eval
['X-RateLimit-Limit: 5', 'X-RateLimit-Limit: 5', 'X-RateLimit-Limit: 5', 'X-RateLimit-Limit: 5', 'X-RateLimit-Limit: 5', 'X-RateLimit-Limit: 5']
--- error_code eval
[200, 200, 200, 200, 200, 429]

=== TEST 2: a per-location burst override narrows the limit
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        ratelimit zone=burst burst=1;
        ratelimit_prefix bo;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
['GET /hit', 'GET /hit', 'GET /hit', 'GET /hit']
--- response_headers eval
['X-RateLimit-Limit: 3', 'X-RateLimit-Limit: 3', 'X-RateLimit-Limit: 3', 'X-RateLimit-Limit: 3']
--- error_code eval
[200, 200, 200, 429]

=== TEST 3: peek reports the burst-widened limit without consuming
--- http_config eval: $::HttpConfig
--- config
    location /peek {
        ratelimit zone=burst quantity=0;
        ratelimit_prefix bp;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /peek
--- response_headers
X-RateLimit-Limit: 5
X-RateLimit-Remaining: 5
!Retry-After
--- error_code: 200

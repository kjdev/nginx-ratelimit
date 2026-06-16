#!/usr/bin/env perl

# Token bucket algorithm (ratelimit_zone ... algo=token_bucket).
# capacity = requests + burst, refill rate = requests / period tokens/sec.
# A fresh bucket starts full, so the first "capacity" requests pass and the
# next is limited. Periods are kept long so refill is negligible during a
# single block.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};
       keepalive 1024;
    }

    ratelimit_zone tb key=\$remote_addr requests=4 period=1m algo=token_bucket;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: peek reports a full bucket without consuming
--- http_config eval: $::HttpConfig
--- config
    location /peek {
        ratelimit zone=tb quantity=0;
        ratelimit_prefix tbpk;
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
X-RateLimit-Limit: 4
X-RateLimit-Remaining: 4
X-RateLimit-Reset: 0
!Retry-After
--- error_code: 200

=== TEST 2: capacity is allowed, then limited
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        ratelimit zone=tb;
        ratelimit_prefix tbhit;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
['GET /hit', 'GET /hit', 'GET /hit', 'GET /hit', 'GET /hit']
--- response_headers eval
['X-RateLimit-Remaining: 3', 'X-RateLimit-Remaining: 2', 'X-RateLimit-Remaining: 1', 'X-RateLimit-Remaining: 0', 'X-RateLimit-Remaining: 0']
--- error_code eval
[200, 200, 200, 200, 429]

=== TEST 3: a quantity beyond capacity is limited up front
--- http_config eval: $::HttpConfig
--- config
    location /big {
        ratelimit zone=tb quantity=5;
        ratelimit_prefix tbq;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /big
--- response_headers
X-RateLimit-Limit: 4
X-RateLimit-Remaining: 4
--- error_code: 429

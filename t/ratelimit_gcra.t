#!/usr/bin/env perl

# GCRA algorithm (ratelimit_zone ... algo=gcra).
# emission interval T = period / requests, burst tolerance = T * (burst + 1).
# A fresh limiter admits "burst + 1" requests at once, then throttles to one
# request per emission interval. limit = burst + 1.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};
       keepalive 1024;
    }

    # 60r/m => T = 1s; burst=3 => 4 requests admitted at once.
    ratelimit_zone gcra key=\$remote_addr requests=60 period=1m burst=3 algo=gcra;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: burst is admitted, then throttled
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        ratelimit zone=gcra;
        ratelimit_prefix gcrahit;
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

=== TEST 2: the limited response carries Limit and Retry-After
--- http_config eval: $::HttpConfig
--- config
    location /retry {
        ratelimit zone=gcra;
        ratelimit_prefix gcraretry;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
['GET /retry', 'GET /retry', 'GET /retry', 'GET /retry', 'GET /retry']
--- response_headers eval
['!Retry-After', '!Retry-After', '!Retry-After', '!Retry-After', 'Retry-After: 1']
--- error_code eval
[200, 200, 200, 200, 429]

=== TEST 3: peek does not consume the limiter
--- http_config eval: $::HttpConfig
--- config
    location /peek {
        ratelimit zone=gcra quantity=0;
        ratelimit_prefix gcrapk;
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
!Retry-After
--- error_code: 200

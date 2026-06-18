#!/usr/bin/env perl

# Sliding window counter (ratelimit_zone ... algo=sliding_window).
# limit = requests + burst. The rate is approximated as
# prev_count * weight + cur_count, weighting the previous window's count by
# the fraction still inside the trailing window. This smooths the boundary
# burst the fixed window allows.
#
# Periods are kept long so each block stays within a single window: the
# previous-window count is then 0 and the estimate equals the current count,
# making the admit/limit thresholds deterministic regardless of where in the
# window the requests land. The boundary smoothing itself depends on the
# server clock and is not asserted here.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};
       keepalive 1024;
    }

    ratelimit_zone sw key=\$remote_addr requests=5 period=1m algo=sliding_window;
    # requests=2 + burst=3 => effective limit 5.
    ratelimit_zone swb key=\$remote_addr requests=2 period=1m burst=3 algo=sliding_window;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: peek reports a full window without consuming
--- http_config eval: $::HttpConfig
--- config
    location /peek {
        ratelimit zone=sw quantity=0;
        ratelimit_prefix swpk;
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
X-RateLimit-Reset: 0
!Retry-After
--- error_code: 200

=== TEST 2: requests are allowed, then limited
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        ratelimit zone=sw;
        ratelimit_prefix swhit;
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
['X-RateLimit-Remaining: 4', 'X-RateLimit-Remaining: 3', 'X-RateLimit-Remaining: 2', 'X-RateLimit-Remaining: 1', 'X-RateLimit-Remaining: 0', 'X-RateLimit-Remaining: 0']
--- error_code eval
[200, 200, 200, 200, 200, 429]

=== TEST 3: allowed responses omit Retry-After, the limited one carries it
--- http_config eval: $::HttpConfig
--- config
    location /retry {
        ratelimit zone=sw;
        ratelimit_prefix swretry;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
['GET /retry', 'GET /retry', 'GET /retry', 'GET /retry', 'GET /retry', 'GET /retry']
--- response_headers eval
['!Retry-After', '!Retry-After', '!Retry-After', '!Retry-After', '!Retry-After', '']
--- response_headers_like eval
['', '', '', '', '', 'Retry-After: \d+']
--- error_code eval
[200, 200, 200, 200, 200, 429]

=== TEST 4: burst widens the limit to requests + burst
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        ratelimit zone=swb;
        ratelimit_prefix swburst;
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

#!/usr/bin/env perl

use Test::Nginx::Socket;

repeat_each(1);

plan tests => repeat_each() * (blocks() * 10 - 2);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};

       # a pool with at most 1024 connections
       keepalive 1024;
    }

    ratelimit_zone quota key=\$remote_addr requests=700 period=3m;
    ratelimit_zone hit key=\$remote_addr requests=4 period=5s;

    error_log logs/error.log debug;
};

no_long_string();
#no_diff();

run_tests();

__DATA__

=== TEST 1: headers (peek without consuming)
--- http_config eval: $::HttpConfig
--- config
    location /quota {
        ratelimit zone=quota quantity=0;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @quota;
    }

    location @quota {
        default_type application/json;
        return 200 '{"X-RateLimit-Limit":$sent_http_x_ratelimit_limit, "X-RateLimit-Remaining":$sent_http_x_ratelimit_remaining, "X-RateLimit-Reset":$sent_http_x_ratelimit_reset}';
    }
--- request
    GET /quota
--- response_headers
X-RateLimit-Limit: 700
X-RateLimit-Remaining: 700
X-RateLimit-Reset: 0
!Retry-After
--- response_body: {"X-RateLimit-Limit":700, "X-RateLimit-Remaining":700, "X-RateLimit-Reset":0}

=== TEST 2: too many requests
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        ratelimit zone=hit;
        ratelimit_prefix a;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @hit;
    }

    location @hit {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
['GET /hit', 'GET /hit', 'GET /hit', 'GET /hit', 'GET /hit']
--- response_headers eval
['X-RateLimit-Remaining: 3', 'X-RateLimit-Remaining: 2', 'X-RateLimit-Remaining: 1', 'X-RateLimit-Remaining: 0', 'X-RateLimit-Remaining: 0']
--- response_body_like eval
['200 OK', '200 OK', '200 OK', '200 OK', '429 Too Many Requests']
--- error_code eval
[200, 200, 200, 200, 429]

=== TEST 3: quantity exceeding the limit is blocked
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        ratelimit zone=hit quantity=5;
        ratelimit_prefix b;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @hit;
    }

    location @hit {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /hit
--- response_headers
X-RateLimit-Limit: 4
X-RateLimit-Remaining: 4
X-RateLimit-Reset: 5
Retry-After: 5
--- response_body_like: 429 Too Many Requests
--- error_code: 429
--- error_log: rate limit exceeded for zone "hit"

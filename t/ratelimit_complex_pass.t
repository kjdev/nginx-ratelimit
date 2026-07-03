#!/usr/bin/env perl

# "ratelimit_pass" also accepts a target with embedded variables (a "complex
# value"), resolved to an upstream at request time instead of at config load
# (see ngx_http_ratelimit_handler()'s ngx_url_t build). That runtime path had
# a latent uninitialized-stack-field bug on nginx < 1.11.6 (see CHANGELOG);
# exercise it end to end, plus its two config-time-valid/request-time-fail
# error branches, so a regression there fails a normal test run.

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

=== TEST 1: a variable ratelimit_pass target resolves to the named upstream
--- http_config eval: $::HttpConfig
--- config
    location /t {
        set $redis_target redis;

        ratelimit zone=byid;
        ratelimit_pass $redis_target;
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

=== TEST 2: an empty variable ratelimit_pass target fails closed with 500
--- http_config eval: $::HttpConfig
--- config
    location /t {
        set $redis_target "";

        ratelimit zone=byid;
        ratelimit_pass $redis_target;
    }
--- request
    GET /t?id=abc
--- error_code: 500
--- error_log
ratelimit: empty "ratelimit_pass" target

=== TEST 3: a variable ratelimit_pass target naming an unknown upstream fails closed with 500
--- http_config eval: $::HttpConfig
--- config
    location /t {
        set $redis_target nonexistent_upstream;

        ratelimit zone=byid;
        ratelimit_pass $redis_target;
    }
--- request
    GET /t?id=abc
--- error_code: 500
--- error_log
ratelimit: upstream "nonexistent_upstream" not found

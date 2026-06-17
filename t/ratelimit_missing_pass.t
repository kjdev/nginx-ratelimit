#!/usr/bin/env perl

# A location with an active "ratelimit zone=" but no resolvable redis target
# must be rejected at config load (nginx -t fails), not crash a worker on the
# first request. A static "ratelimit_pass" satisfies the requirement; so does a
# target inherited from an enclosing scope.

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

=== TEST 1: zone without ratelimit_pass is rejected at config load
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=byid;
    }
--- must_die
--- error_log
ratelimit zone "byid" requires "ratelimit_pass"

=== TEST 2: a static ratelimit_pass satisfies the requirement
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=byid;
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

=== TEST 3: a ratelimit_pass inherited from an enclosing scope is accepted
--- http_config eval: $::HttpConfig
--- config
    ratelimit_pass redis;

    location /t {
        ratelimit zone=byid;
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

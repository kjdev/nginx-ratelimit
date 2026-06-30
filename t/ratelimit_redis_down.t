#!/usr/bin/env perl

# Behaviour when the limiter cannot reach a verdict. The default is fail-closed:
# an unreachable Redis rejects the request with a 5xx rather than silently
# allowing it (the upstream points at a dead port so the connect is refused).
# "ratelimit_on_error allow" flips this to fail-open for transport failures
# only: the request passes. A contract violation (a malformed script reply,
# surfaced as 500) stays fail-closed even under "allow", so a broken setup is
# never silently let through.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis_dead {
       server 127.0.0.1:1;
       keepalive 4;
    }

    ratelimit_zone down key=\$remote_addr requests=10 period=1m;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: an unreachable Redis fails the request closed
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=down;
        ratelimit_prefix downz;
        ratelimit_pass redis_dead;
        ratelimit_connect_timeout 200ms;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /t
--- error_code: 503
--- response_body_unlike: 200 OK

=== TEST 2: "ratelimit_on_error allow" fails open when Redis is unreachable
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=down;
        ratelimit_prefix downz;
        ratelimit_pass redis_dead;
        ratelimit_connect_timeout 200ms;
        ratelimit_on_error allow;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /t
--- error_code: 200
--- response_body
200 OK
--- response_headers
! X-RateLimit-Limit
! X-RateLimit-Remaining
! X-RateLimit-Reset
! Retry-After

=== TEST 3: "allow" still fails closed on a contract violation (500)
--- http_config
    upstream redis {
       server 127.0.0.1:$TEST_NGINX_REDIS_PORT;
       keepalive 1024;
    }

    ratelimit_zone broken key=$remote_addr requests=2 period=1m algo=custom
        script=html/bad.lua;

    error_log logs/error.log debug;
--- config
    location /t {
        ratelimit zone=broken;
        ratelimit_prefix brokenz;
        ratelimit_pass redis;
        ratelimit_on_error allow;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- user_files
>>> bad.lua
return {1, 2, 3}
--- request
    GET /t
--- error_code: 500
--- response_body_unlike: 200 OK

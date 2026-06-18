#!/usr/bin/env perl

# Requires a redis/valkey started with "--requirepass testpass" on
# TEST_NGINX_REDIS_AUTH_PORT (default 6390). The AUTH/SELECT prelude is
# prepended to every request, so a reused keepalive connection re-establishes
# its DB and auth identity rather than inheriting a previous request's.

use Test::Nginx::Socket;

repeat_each(1);

plan tests => repeat_each() * 10;

$ENV{TEST_NGINX_REDIS_AUTH_PORT} ||= 6390;

our $HttpConfig = qq{
    upstream redis_auth {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_AUTH_PORT};
       keepalive 16;
    }

    ratelimit_zone authz key=\$remote_addr requests=3 period=10s;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: AUTH + SELECT on a fresh connection, then limit
--- http_config eval: $::HttpConfig
--- config
    location /a {
        ratelimit zone=authz;
        ratelimit_prefix auth;
        ratelimit_pass redis_auth;
        ratelimit_password testpass;
        ratelimit_database 2;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
['GET /a', 'GET /a', 'GET /a', 'GET /a']
--- error_code eval
[200, 200, 200, 429]
--- response_body_like eval
['200 OK', '200 OK', '200 OK', '429 Too Many Requests']

=== TEST 2: wrong password fails the request
--- http_config eval: $::HttpConfig
--- config
    location /b {
        ratelimit zone=authz;
        ratelimit_prefix authbad;
        ratelimit_pass redis_auth;
        ratelimit_password wrongpass;
        ratelimit_database 2;
    }
--- request
    GET /b
--- error_code: 500
--- error_log: redis auth/select error reply

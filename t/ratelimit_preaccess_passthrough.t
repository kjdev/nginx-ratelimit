#!/usr/bin/env perl

# The PREACCESS phase checker (ngx_http_core_generic_phase()) treats a
# handler's NGX_OK as "cut the whole phase short", not "this handler
# succeeded": r->phase_handler jumps straight to the first handler of the
# *next* phase. A request this module allows must therefore return
# NGX_DECLINED so sibling PREACCESS handlers -- limit_req here, standing in
# for any co-located module -- still run. This covers both the plain allow
# path and the "ratelimit_on_error allow" fail-open path.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};
       keepalive 1024;
    }

    upstream redis_dead {
       server 127.0.0.1:1;
       keepalive 4;
    }

    # Generous enough that it never trips within either test below; the
    # point is to observe limit_req's own, much tighter budget.
    ratelimit_zone rz key=\$remote_addr requests=1000 period=1m;
    ratelimit_zone down key=\$remote_addr requests=1000 period=1m;

    limit_req_zone \$binary_remote_addr zone=lr:1m rate=1r/m;
    limit_req_zone \$binary_remote_addr zone=lr2:1m rate=1r/m;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: ratelimit allowing a request still lets limit_req run
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=rz;
        ratelimit_prefix pt;
        ratelimit_pass redis;

        limit_req zone=lr burst=1 nodelay;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
["GET /t", "GET /t", "GET /t"]
--- error_code eval
[200, 200, 503]

=== TEST 2: "ratelimit_on_error allow" fail-open still lets limit_req run
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=down;
        ratelimit_prefix pt2;
        ratelimit_pass redis_dead;
        ratelimit_connect_timeout 200ms;
        ratelimit_on_error allow;

        limit_req zone=lr2 burst=1 nodelay;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
["GET /t", "GET /t", "GET /t"]
--- error_code eval
[200, 200, 503]

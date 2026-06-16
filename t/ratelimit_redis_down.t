#!/usr/bin/env perl

# When Redis is unreachable the limiter fails closed: the request is rejected
# with a 5xx rather than silently allowed. The upstream points at a dead port
# so the connect is refused. (A "header already sent" alert from the upstream
# finalize path is a known, harmless issue tracked in issues/001.)

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

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

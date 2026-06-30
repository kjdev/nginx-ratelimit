#!/usr/bin/env perl

# A reply truncated mid-array must not be accepted as a verdict. The mock
# Redis sends the multibulk header and the first integer ("*5\r\n:0\r\n", the
# allowed=0 field) then drops the connection. The parser never reaches its
# done label, so the read is a premature close, i.e. a transport failure
# mapped to 502: fail-closed by default, fail-open only under
# "ratelimit_on_error allow". Before the fix the half-read reply was silently
# treated as allowed regardless of the on_error setting.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

our $HttpConfig = qq{
    upstream redis_mock {
       server 127.0.0.1:8967;
    }

    ratelimit_zone trunc key=\$remote_addr requests=10 period=1m;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: a reply truncated after the array header fails closed (502)
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=trunc;
        ratelimit_prefix truncz;
        ratelimit_pass redis_mock;
    }
--- request
    GET /t
--- tcp_listen: 8967
--- tcp_reply eval
"*5\r\n:0\r\n"
--- error_code: 502
--- response_body_unlike: 200 OK

=== TEST 2: "ratelimit_on_error allow" fails open on a truncated reply
--- http_config eval: $::HttpConfig
--- config
    location /t {
        ratelimit zone=trunc;
        ratelimit_prefix truncz;
        ratelimit_pass redis_mock;
        ratelimit_on_error allow;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /t
--- tcp_listen: 8967
--- tcp_reply eval
"*5\r\n:0\r\n"
--- error_code: 200
--- response_body
200 OK

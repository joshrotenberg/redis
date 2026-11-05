# Consumer group filters: XGROUP CREATE ... FILTER <expr> | ELSE,
# XGROUP SETFILTER, XREADGROUP ... MAXSCAN.

# IDs of the entries in a one-stream XREADGROUP reply.
proc filter_ids {reply} {
    set out {}
    foreach entry [lindex $reply 0 1] {lappend out [lindex $entry 0]}
    return $out
}

# A group's XINFO GROUPS entry, or "" if there is no such group.
proc filter_group {key name} {
    foreach g [r XINFO GROUPS $key] {
        if {[dict get $g name] eq $name} {return $g}
    }
    return ""
}

proc filter_ticks {} {
    r DEL ticks
    r XADD ticks 1-1 type fx sym EURUSD px 1.08
    r XADD ticks 1-2 type stock sym AAPL px 190
    r XADD ticks 1-3 type bond sym UST10Y px 98.5
    r XADD ticks 1-4 type stock sym MSFT px 410
}

# Positions (1-based) of the entries in 'entries' that 'filter' delivers.
proc filter_matching {filter entries} {
    r DEL m
    set j 0
    foreach e $entries {
        incr j
        r XADD m 1-$j {*}$e
    }
    r XGROUP CREATE m g 0 FILTER $filter
    set out {}
    foreach id [filter_ids [r XREADGROUP GROUP g c STREAMS m >]] {
        lappend out [lindex [split $id -] 1]
    }
    return $out
}

start_server {
    tags {"stream"}
} {
    arm_stream_stats_assert

    test {XGROUP CREATE FILTER: a group receives only the entries it matches} {
        filter_ticks
        r XGROUP CREATE ticks fx 0 FILTER {type = fx}
        r XGROUP CREATE ticks big 0 FILTER {type = stock AND px > 200}
        assert_equal {1-1} [filter_ids [r XREADGROUP GROUP fx c STREAMS ticks >]]
        assert_equal {1-4} [filter_ids [r XREADGROUP GROUP big c STREAMS ticks >]]
        # Caught up, exactly as an unfiltered group would be.
        assert_equal {} [r XREADGROUP GROUP fx c STREAMS ticks >]
    }

    test {Filtered groups: the PEL holds deliveries only, the cursor passes every entry examined} {
        assert_equal {1 1-1 1-1 {{c 1}}} [r XPENDING ticks fx]
        set g [filter_group ticks fx]
        assert_equal 1-4 [dict get $g last-delivered-id]
        assert_equal 4 [dict get $g entries-read]
        assert_equal 0 [dict get $g lag]
        assert_equal {type = fx} [dict get $g filter]
        assert_equal 10000 [dict get $g maxscan]
        # XACK, XCLAIM and XAUTOCLAIM work on the delivered entry as usual.
        assert_equal 1 [r XACK ticks fx 1-1]
        assert_equal 0 [lindex [r XPENDING ticks fx] 0]
    }

    test {XGROUP CREATE ELSE: the group receives what no other filtered group matches} {
        r XGROUP CREATE ticks other 0 ELSE
        r XGROUP CREATE ticks plain 0
        assert_equal {1-2 1-3} [filter_ids [r XREADGROUP GROUP other c STREAMS ticks >]]
        # Unfiltered groups don't count as coverage, and see everything.
        assert_equal {1-1 1-2 1-3 1-4} [filter_ids [r XREADGROUP GROUP plain c STREAMS ticks >]]
        assert_error {*already has an ELSE group*} {r XGROUP CREATE ticks other2 0 ELSE}
        assert_error {*already has an ELSE group*} {r XGROUP SETFILTER ticks fx ELSE}
        # FILTER ELSE means the same.
        r XGROUP DESTROY ticks other
        r XGROUP CREATE ticks other 0 FILTER ELSE
        assert_equal ELSE [dict get [filter_group ticks other] filter]
    }

    test {XGROUP CREATE FILTER: an invalid expression is rejected with its byte offset} {
        assert_error {ERR invalid filter: syntax error at byte 21: expected a value} \
            {r XGROUP CREATE ticks bad 0 FILTER {type = stock AND px >}}
        assert_error {*byte 4: expected an operator, IN, NOT or EXISTS after field 'nope'} \
            {r XGROUP SETFILTER ticks fx nope}
        assert_error {*is too large for f64*quote it to compare bytewise} \
            {r XGROUP CREATE ticks bad 0 FILTER "x = [string repeat 9 309]"}
        assert_error {*nested deeper than 16 levels} \
            {r XGROUP CREATE ticks bad 0 FILTER "[string repeat ( 17]a = 1[string repeat ) 17]"}
        assert_equal {} [filter_group ticks bad]
        # Nothing is created on error, not even with MKSTREAM.
        assert_error {*invalid filter*} {r XGROUP CREATE nostream g 0 MKSTREAM FILTER {a =}}
        assert_equal 0 [r EXISTS nostream]
    }

    test {Filter options are validated} {
        assert_error {*MAXSCAN must be a positive integer} {r XREADGROUP GROUP fx c MAXSCAN 0 STREAMS ticks >}
        assert_error {*only supported by XREADGROUP*} {r XREAD MAXSCAN 5 STREAMS ticks 0}
        assert_error {*MAXSCAN must be a positive integer or DEFAULT} {r XGROUP SETFILTER ticks fx {type = fx} MAXSCAN -1}
        assert_error {*NOGROUP*} {r XGROUP SETFILTER ticks nosuchgroup {a = 1}}
        assert_error {*unknown subcommand or wrong number of arguments for 'SETID'*} \
            {r XGROUP SETID ticks fx 0 FILTER {a = 1}}
    }

    test {XREADGROUP MAXSCAN: an empty entry list when the budget runs out before a match} {
        r DEL s
        r XADD s 1-1 type bond
        r XADD s 1-2 type bond
        r XADD s 1-3 type fx
        r XGROUP CREATE s fx 0 FILTER {type = fx}
        assert_equal {{s {}}} [r XREADGROUP GROUP fx c MAXSCAN 2 STREAMS s >]
        assert_equal 1-2 [dict get [filter_group s fx] last-delivered-id]
        assert_equal {1-3} [filter_ids [r XREADGROUP GROUP fx c MAXSCAN 2 STREAMS s >]]
        assert_equal {} [r XREADGROUP GROUP fx c MAXSCAN 2 STREAMS s >]
    }

    test {The group's MAXSCAN applies when the read sets none, the config when neither does} {
        r DEL s
        for {set j 1} {$j <= 5} {incr j} {r XADD s 1-$j type bond}
        r XADD s 1-6 type fx
        r XGROUP CREATE s fx 0 FILTER {type = fx} MAXSCAN 3
        assert_equal 3 [dict get [filter_group s fx] maxscan]
        assert_equal {{s {}}} [r XREADGROUP GROUP fx c STREAMS s >]
        assert_equal {1-6} [filter_ids [r XREADGROUP GROUP fx c STREAMS s >]]
        r XGROUP SETFILTER s fx {type = fx} MAXSCAN DEFAULT
        assert_equal 10000 [dict get [filter_group s fx] maxscan]
        r CONFIG SET stream-filter-max-scan 7
        assert_equal 7 [dict get [filter_group s fx] maxscan]
        r CONFIG SET stream-filter-max-scan 10000
    }

    test {XREADGROUP COUNT counts deliveries, not entries examined} {
        r DEL s
        for {set j 1} {$j <= 20} {incr j} {r XADD s 1-$j n $j parity [expr {$j % 2}]}
        r XGROUP CREATE s odd 0 FILTER {parity = 1}
        assert_equal {1-1 1-3 1-5} [filter_ids [r XREADGROUP GROUP odd c COUNT 3 STREAMS s >]]
        assert_equal 1-5 [dict get [filter_group s odd] last-delivered-id]
        assert_equal {1-7 1-9} [filter_ids [r XREADGROUP GROUP odd c COUNT 2 STREAMS s >]]
    }

    test {Filters work across listpack nodes} {
        r DEL s
        r CONFIG SET stream-node-max-entries 7
        for {set j 1} {$j <= 500} {incr j} {
            if {$j % 3 == 0} {r XADD s * kind three n $j} else {r XADD s * n $j}
        }
        r CONFIG SET stream-node-max-entries 100
        r XGROUP CREATE s three 0 FILTER {kind = three}
        r XGROUP CREATE s rest 0 ELSE
        assert_equal 166 [llength [filter_ids [r XREADGROUP GROUP three c STREAMS s >]]]
        assert_equal 334 [llength [filter_ids [r XREADGROUP GROUP rest c STREAMS s >]]]
        assert_equal 166 [lindex [r XPENDING s three] 0]
    }

    test {XGROUP SETFILTER changes what the group receives from the next delivery} {
        filter_ticks
        r XGROUP CREATE ticks g 0 FILTER {type = fx}
        assert_equal {1-1} [filter_ids [r XREADGROUP GROUP g c COUNT 1 STREAMS ticks >]]
        r XGROUP SETFILTER ticks g {type IN (stock, bond)}
        assert_equal {1-2 1-3 1-4} [filter_ids [r XREADGROUP GROUP g c STREAMS ticks >]]
        r XGROUP SETFILTER ticks g NONE
        r XADD ticks 2-1 type fx
        assert_equal {2-1} [filter_ids [r XREADGROUP GROUP g c STREAMS ticks >]]
        # Without a filter, XINFO GROUPS reports exactly the fields it did before.
        assert_equal {name consumers pending last-delivered-id entries-read lag} \
            [dict keys [filter_group ticks g]]
    }

    test {Reading a filtered group's history ignores the filter} {
        filter_ticks
        r XGROUP CREATE ticks g 0 FILTER {type = stock}
        r XREADGROUP GROUP g c STREAMS ticks >
        assert_equal {1-2 1-4} [filter_ids [r XREADGROUP GROUP g c STREAMS ticks 0]]
    }

    test {Filter grammar: the literal decides the comparison type} {
        assert_equal {1} [filter_matching {px > 9} {{px 80} {px 9} {px abc}}]
        assert_equal {2} [filter_matching {px > "9"} {{px 80} {px 95}}]
        assert_equal {1 2} [filter_matching {id = 7} {{id 007} {id 7.0} {id 7a}}]
        assert_equal {1} [filter_matching {id = "007"} {{id 007} {id 7}}]
        assert_equal {2} [filter_matching {x != 5} {{x abc} {x 6}}]
        assert_equal {1} [filter_matching {n = 9007199254740993} {{n 9007199254740993} {n 9007199254740992}}]
    }

    test {Filter grammar: missing fields, IN, EXISTS and substrings} {
        assert_equal {1} [filter_matching {level != debug} {{level info} {type fx} {level debug}}]
        assert_equal {2} [filter_matching {level NOT EXISTS} {{level info} {type fx}}]
        assert_equal {1} [filter_matching {sym NOT IN (a, b)} {{sym c} {type fx} {sym a}}]
        assert_equal {1 2} [filter_matching {code IN (200, ok)} {{code 200.0} {code ok} {code OK}}]
        assert_equal {1} [filter_matching {z IN (0)} {{z -0} {z 1}}]
        assert_equal {2} [filter_matching {sym ~ "1.*"} {{sym UST10Y} {sym x1.*y}}]
        assert_equal {1} [filter_matching {sym ~ 10Y} {{sym UST10Y} {sym UST5Y}}]
        assert_equal {1 3} [filter_matching {type = fx OR type = bond AND rating = AAA} \
                                {{type fx} {type bond rating BB} {type bond rating AAA}}]
        assert_equal {3} [filter_matching {(type = fx OR type = bond) AND rating = AAA} \
                              {{type fx} {type bond rating BB} {type bond rating AAA}}]
        assert_equal {1} [filter_matching {"my field" EXISTS and Type = FX} {{{my field} 1 Type FX}}]
    }

    test {Filter grammar: integer-encoded values and repeated fields} {
        # Both values are integer-encoded in the listpack.
        assert_equal {1} [filter_matching {a = 1 AND b = 2} {{a 1 b 2} {a 2 b 1}}]
        # The first occurrence of a field is its value.
        assert_equal {} [filter_matching {a = 2} {{a 1 a 2}}]
        assert_equal {1} [filter_matching {a = 1} {{a 1 a 2}}]
    }

    test {Filters read values by field position, node by node} {
        r DEL s
        set node_max [lindex [r CONFIG GET stream-node-max-entries] 1]
        r CONFIG SET stream-node-max-entries 3
        # Node 1 lists a, b; node 2 lists b, a; node 3 has no b. An entry
        # whose fields differ from its node's first entry (1-2, 1-6) carries
        # its own field names.
        r XADD s 1-1 a 1 b 2
        r XADD s 1-2 b 4 a 3
        r XADD s 1-3 a 5 b 6
        r XADD s 1-4 b 7 a 8
        r XADD s 1-5 b 9 a 10
        r XADD s 1-6 a 11
        r XADD s 1-7 a 12 c 13
        r XADD s 1-8 a 14 c 15
        set out {}
        foreach {name filter} {
            g1 {a >= 3 AND a <= 10}
            g2 {b = 9}
            g3 {a = 11}
            g4 {b NOT EXISTS}
            g5 {c = 15 AND a = 14}
        } {
            r XGROUP CREATE s $name 0 FILTER $filter
            lappend out [filter_ids [r XREADGROUP GROUP $name c STREAMS s >]]
        }
        r CONFIG SET stream-node-max-entries $node_max
        assert_equal {{1-2 1-3 1-4 1-5} 1-5 1-6 {1-6 1-7 1-8} 1-8} $out
    }

    test {Filters read the first of a node's repeated field names} {
        r DEL s
        r XADD s 1-1 a 1 a 2
        r XADD s 1-2 a 3 a 4
        r XGROUP CREATE s g1 0 FILTER {a = 3}
        r XGROUP CREATE s g2 0 FILTER {a = 4}
        assert_equal {1-2} [filter_ids [r XREADGROUP GROUP g1 c STREAMS s >]]
        assert_equal {} [filter_ids [r XREADGROUP GROUP g2 c STREAMS s >]]
    }

    test {A filter that names more than 16 fields} {
        r DEL s
        set fields {}
        set clauses {}
        for {set j 1} {$j <= 17} {incr j} {
            lappend fields f$j $j
            lappend clauses "f$j = $j"
        }
        r XADD s 1-1 {*}$fields
        r XADD s 1-2 {*}$fields
        lset fields end 0
        r XADD s 1-3 {*}$fields
        r XGROUP CREATE s g 0 FILTER [join $clauses " AND "]
        assert_equal {1-1 1-2} [filter_ids [r XREADGROUP GROUP g c STREAMS s >]]
    }

    test {A filter's field positions are not reused for a new node} {
        r DEL s
        r XGROUP CREATE s g $ MKSTREAM FILTER {a = 2}
        r XADD s 1-1 a 2 b 1
        assert_equal {1-1} [filter_ids [r XREADGROUP GROUP g c STREAMS s >]]
        # Free the node, then add one of the same size with the fields
        # swapped: the allocator may hand back the same address.
        r XTRIM s MAXLEN 0
        r XADD s 1-2 b 2 a 1
        r XADD s 1-3 b 1 a 2
        assert_equal {1-3} [filter_ids [r XREADGROUP GROUP g c STREAMS s >]]
    }

    test {COPY carries a group's filter} {
        filter_ticks
        r XGROUP CREATE ticks fx 0 FILTER {type = fx} MAXSCAN 50
        r DEL ticks2
        r COPY ticks ticks2
        assert_equal [r XINFO GROUPS ticks] [r XINFO GROUPS ticks2]
        assert_equal {1-1} [filter_ids [r XREADGROUP GROUP fx c STREAMS ticks2 >]]
    }

    test {DUMP and RESTORE carry a group's filter} {
        r DEL ticks2
        r RESTORE ticks2 0 [r DUMP ticks]
        assert_equal [r XINFO GROUPS ticks] [r XINFO GROUPS ticks2]
    }

    test {Filters survive DEBUG RELOAD} {
        filter_ticks
        r XGROUP CREATE ticks fx 0 FILTER {type = fx} MAXSCAN 50
        r XGROUP CREATE ticks other 0 ELSE
        r XGROUP CREATE ticks plain 0
        set before [r XINFO GROUPS ticks]
        r DEBUG RELOAD
        assert_equal $before [r XINFO GROUPS ticks]
        assert_equal {1-2 1-3 1-4} [filter_ids [r XREADGROUP GROUP other c STREAMS ticks >]]
    } {} {needs:debug}

    test {Blocking XREADGROUP on a filtered group wakes only for a match} {
        r DEL s
        r XGROUP CREATE s big $ MKSTREAM FILTER {px > 200}
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP big c BLOCK 0 STREAMS s >
        wait_for_blocked_clients_count 1
        r XADD s 1-1 px 5
        # Still blocked, and the read did not run again: it would have moved
        # the cursor past the entry.
        assert_equal 1 [s blocked_clients]
        assert_equal 0-0 [dict get [filter_group s big] last-delivered-id]
        r XADD s 1-2 px 500
        assert_equal {{s {{1-2 {px 500}}}}} [$rd read]
        assert_equal 1-2 [dict get [filter_group s big] last-delivered-id]
        $rd close
    }

    test {One matching entry wakes one of a filtered group's blocked readers} {
        r DEL s
        r XGROUP CREATE s big $ MKSTREAM FILTER {px > 200}
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        $rd1 XREADGROUP GROUP big c1 BLOCK 0 STREAMS s >
        $rd2 XREADGROUP GROUP big c2 BLOCK 0 STREAMS s >
        wait_for_blocked_clients_count 2
        r XADD s 1-1 px 5
        assert_equal 2 [s blocked_clients]
        r XADD s 1-2 px 500
        assert_equal {{s {{1-2 {px 500}}}}} [$rd1 read]
        assert_equal 1 [s blocked_clients]
        r XADD s 1-3 px 600
        assert_equal {{s {{1-3 {px 600}}}}} [$rd2 read]
        $rd1 close
        $rd2 close
    }

    test {A blocked filtered read returns an empty entry list once its scan budget runs out} {
        r DEL s
        r XGROUP CREATE s big $ MKSTREAM FILTER {px > 200}
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP big c MAXSCAN 3 BLOCK 0 STREAMS s >
        wait_for_blocked_clients_count 1
        # Three entries fit the budget; the fourth is one more than it allows.
        for {set j 1} {$j <= 3} {incr j} {r XADD s 1-$j px 5}
        assert_equal 1 [s blocked_clients]
        r XADD s 1-4 px 5
        assert_equal {{s {}}} [$rd read]
        assert_equal 1-3 [dict get [filter_group s big] last-delivered-id]
        $rd close
    }

    test {A blocked filtered read checks every entry added at once, across listpack nodes} {
        r DEL s
        set node_max [lindex [r CONFIG GET stream-node-max-entries] 1]
        r CONFIG SET stream-node-max-entries 2
        r XGROUP CREATE s big $ MKSTREAM FILTER {px > 200}
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP big c BLOCK 0 STREAMS s >
        wait_for_blocked_clients_count 1
        r MULTI
        for {set j 1} {$j <= 5} {incr j} {r XADD s 1-$j px 5}
        r EXEC
        assert_equal 1 [s blocked_clients]
        r MULTI
        r XADD s 1-6 px 500
        for {set j 7} {$j <= 11} {incr j} {r XADD s 1-$j px 5}
        r EXEC
        assert_equal {{s {{1-6 {px 500}}}}} [$rd read]
        $rd close
        r CONFIG SET stream-node-max-entries $node_max
    }

    test {A blocked ELSE reader wakes only for entries no filtered group takes} {
        r DEL s
        r XGROUP CREATE s hi $ MKSTREAM FILTER {px > 200}
        r XGROUP CREATE s rest $ ELSE
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP rest c BLOCK 0 STREAMS s >
        wait_for_blocked_clients_count 1
        r XADD s 1-1 px 500
        assert_equal 1 [s blocked_clients]
        r XADD s 1-2 px 5
        assert_equal {{s {{1-2 {px 5}}}}} [$rd read]
        $rd close
    }

    test {XGROUP SETFILTER wakes a blocked reader that the new filter serves} {
        r DEL s
        r XGROUP CREATE s big $ MKSTREAM FILTER {px > 200}
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP big c BLOCK 0 STREAMS s >
        wait_for_blocked_clients_count 1
        r XADD s 1-1 px 5
        assert_equal 1 [s blocked_clients]
        r XGROUP SETFILTER s big {px < 10}
        assert_equal {{s {{1-1 {px 5}}}}} [$rd read]
        $rd close
    }

    test {XGROUP SETID while a filtered reader is blocked: the next entry checks from the new cursor} {
        r DEL s
        r XADD s 1-1 px 500
        r XGROUP CREATE s big $ FILTER {px > 200}
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP big c BLOCK 0 STREAMS s >
        wait_for_blocked_clients_count 1
        r XGROUP SETID s big 0
        r XADD s 1-2 px 5
        assert_equal {{s {{1-1 {px 500}}}}} [$rd read]
        $rd close
    }

    test {XGROUP DESTROY unblocks a filtered group's reader with NOGROUP} {
        r DEL s
        r XGROUP CREATE s big $ MKSTREAM FILTER {px > 200}
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP big c BLOCK 0 STREAMS s >
        wait_for_blocked_clients_count 1
        r XGROUP DESTROY s big
        assert_error "*NOGROUP*" {$rd read}
        $rd close
    }

    test {Blocking XREADGROUP on a filtered group keeps its timeout} {
        r DEL s
        r XGROUP CREATE s big $ MKSTREAM FILTER {px > 200}
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP big c BLOCK 400 STREAMS s >
        wait_for_blocked_clients_count 1
        # Entries that don't match leave the read blocked; none may restart
        # its timeout.
        for {set j 0} {$j < 10} {incr j} {
            r XADD s * px 1
            after 70
        }
        assert_equal 0 [s blocked_clients]
        assert_equal {} [$rd read]
        $rd close
    }
}

start_server {tags {"stream needs:debug"} overrides {appendonly yes aof-use-rdb-preamble no appendfsync always}} {
    test {Filtered reads replay from the AOF} {
        r DEL s
        for {set j 1} {$j <= 10} {incr j} {r XADD s 1-$j n $j parity [expr {$j % 2}]}
        r XGROUP CREATE s odd 0 FILTER {parity = 1} MAXSCAN 4
        r XGROUP CREATE s rest 0 ELSE
        r XREADGROUP GROUP odd c COUNT 2 STREAMS s >
        r XREADGROUP GROUP rest c MAXSCAN 1 STREAMS s >
        r XGROUP SETFILTER s rest {parity = 0}
        r XREADGROUP GROUP rest c STREAMS s >
        set before [r XINFO GROUPS s]
        set pending [r XPENDING s odd]
        r debug loadaof
        assert_equal $before [r XINFO GROUPS s]
        assert_equal $pending [r XPENDING s odd]
    }

    test {An AOF rewrite keeps filters, ELSE groups and scan budgets} {
        set before [r XINFO GROUPS s]
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal $before [r XINFO GROUPS s]
    }
}

start_server {tags {"stream repl external:skip"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    start_server {} {
        set replica [srv 0 client]

        test {A replica keeps filtered groups in step with the master} {
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
            $master DEL s
            for {set j 1} {$j <= 30} {incr j} {$master XADD s 1-$j n $j mod [expr {$j % 3}]}
            $master XGROUP CREATE s zero 0 FILTER {mod = 0}
            $master XGROUP CREATE s rest 0 ELSE
            $master XREADGROUP GROUP zero c COUNT 4 STREAMS s >
            # Only skipped entries: the cursor moves, nothing is delivered.
            $master XREADGROUP GROUP zero c MAXSCAN 2 STREAMS s >
            $master XREADGROUP GROUP rest c COUNT 5 STREAMS s >
            wait_for_ofs_sync $master $replica
            assert_equal [$master XINFO GROUPS s] [$replica XINFO GROUPS s]
            # The summary form: the extended one reports idle times.
            assert_equal [$master XPENDING s zero] [$replica XPENDING s zero]
            assert_equal [$master XPENDING s rest] [$replica XPENDING s rest]
        }

        test {A full resync carries filters to the replica} {
            set full_syncs [s -1 sync_full]
            $replica replicaof no one
            $replica flushall
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
            wait_for_ofs_sync $master $replica
            assert {[s -1 sync_full] > $full_syncs}
            assert_equal [$master XINFO GROUPS s] [$replica XINFO GROUPS s]
        }

        test {A promoted replica keeps filtering} {
            $replica replicaof no one
            set ids [filter_ids [$replica XREADGROUP GROUP zero c STREAMS s >]]
            foreach id $ids {
                set n [lindex [split $id -] 1]
                assert_equal 0 [expr {$n % 3}]
            }
            assert {[llength $ids] > 0}
        }
    }
}

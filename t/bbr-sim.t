#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;

$ENV{BINARY_DIR} ||= ".";
my $simulator = "$ENV{BINARY_DIR}/simulator";

# Runs the simulator for `length` seconds. Returns each flow's delivery rate over the last `window`
# seconds in bytes per second, in the order the flows were given, and the mean bottleneck queue in
# bytes.
sub simulate {
    my ($seed, $length, $window, @args) = @_;
    my $from = 1000 + $length - $window; # the simulator's clock starts at 1000 seconds
    my (%start, %last);
    my $queue_sum = 0;
    my $queue_cnt = 0;

    open my $fh, "-|", "env", "QUICLY_SIM_SEED=$seed", $simulator, "-l", $length, @args
        or die "failed to run $simulator: $!";
    while (my $line = <$fh>) {
        if ($line =~ /^\{"bytes-available": (\d+), "at": ([\d.]+), "packet-src": (\d+)\}/) {
            $start{$3} = $1 if $2 <= $from;
            $last{$3} = $1;
        } elsif ($line =~ /"queue-size": (\d+)/) {
            $queue_sum += $1;
            ++$queue_cnt;
        }
    }
    close $fh;
    is $?, 0, "simulator exits cleanly";

    my @rates = map { ($last{$_} - ($start{$_} // 0)) / $window } sort { $a <=> $b } keys %last;
    my $queue = $queue_cnt != 0 ? $queue_sum / $queue_cnt : 0;
    my $rates_str = join " ", map { sprintf "%.0f", $_ } @rates;
    note sprintf "seed %d: %s B/s, mean queue %.0f B", $seed, $rates_str, $queue;
    return (\@rates, $queue);
}

my @seeds = (1, 2, 3);

# A wrong delivery rate shows up here: missing snapshots starve the flow, and a missing estimate
# leaves a standing queue.
subtest "alone at a 1xBDP bottleneck" => sub {
    for my $seed (@seeds) {
        my ($rates, $queue) = simulate($seed, 20, 15, qw(-b 1000000 -q 0.02 -- -c bbr -d 0.01));
        cmp_ok $rates->[0], ">=", 0.9 * 1e6, "utilization (seed $seed)";
        cmp_ok $queue, "<=", 0.5 * 20000, "mean queue under half a BDP (seed $seed)";
    }
};

subtest "random loss of 5%" => sub {
    for my $seed (@seeds) {
        my ($rates) = simulate($seed, 20, 15, qw(-b 1000000 -q 0.02 -r 0.05 -- -c bbr -d 0.01));
        cmp_ok $rates->[0], ">=", 0.8 * 1e6, "utilization (seed $seed)";
    }
};

subtest "against cubic at a 5xBDP bottleneck" => sub {
    for my $seed (@seeds) {
        my ($rates) =
            simulate($seed, 30, 20, qw(-b 1000000 -q 0.1 -- -c bbr -d 0.01 -- -c cubic -d 0.01));
        cmp_ok $rates->[0], ">=", 0.25 * 1e6, "bbr share (seed $seed)";
        cmp_ok $rates->[1], ">=", 0.25 * 1e6, "cubic share (seed $seed)";
    }
};

subtest "two flows, the second joining after 5 seconds" => sub {
    for my $seed (@seeds) {
        my ($rates) = simulate($seed, 30, 20,
            qw(-b 1000000 -q 0.02 -- -c bbr -d 0.01 -- -c bbr -d 0.01 -s 5));
        cmp_ok $rates->[0], ">=", 0.2 * 1e6, "first flow share (seed $seed)";
        cmp_ok $rates->[1], ">=", 0.2 * 1e6, "second flow share (seed $seed)";
    }
};

# With a sub-millisecond RTT, a fixed 1 ms floor on the sample interval discards most samples.
subtest "60 us RTT" => sub {
    for my $seed (@seeds) {
        my ($rates) = simulate($seed, 5, 4, qw(-b 10000000 -q 0.001 -- -c bbr -d 0.00003));
        cmp_ok $rates->[0], ">=", 0.91 * 1e7, "utilization (seed $seed)";
    }
};

done_testing;

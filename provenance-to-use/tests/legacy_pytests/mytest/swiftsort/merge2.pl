#!/usr/bin/perl -w
#
# parameters: file1 [file2]
use strict;

sub merge {
  my ($f1, $f2) = @_;
  my ($FILE1, $FILE2);
  open FILE1, $f1 or die $!;
  open FILE2, $f2 or die $!;
  
  my $line1 = <FILE1>;
  my $line2 = <FILE2>;

  while (1) {
    if ($line1 && $line2) {
      if ($line1 < $line2) {
        print "$line1";
        $line1 = <FILE1>;
      } else {
        print "$line2";
        $line2 = <FILE2>;
      }
    } else {
      if ($line1) {
        print "$line1";
        $line1 = <FILE1>;
      }
      if ($line2) {
        print "$line2";
        $line2 = <FILE2>;
      } 
      if (!$line1 && !$line2) {
        last;
      }
    }
  }
  
  close(FILE2);
  close(FILE1);
}

sub cat {
  my ($f1) = @_;
  my $FILE1;
  open FILE1, $f1 or die $!;
  while (<FILE1>) {
    print "$_";
  }
  close(FILE1);
}

my $num = $#ARGV + 1;
if ($num == 2) {
  merge($ARGV[0],$ARGV[1]);
} elsif ($num == 1) {
  cat($ARGV[0]);
} else {
  print "argc: $num\n";
  print @ARGV;
}

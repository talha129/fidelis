type inputfile {}
type sortedfile {}
type textfile {}

(sortedfile t) sort (string f, textfile fileList) {
  app {
    sort "-n" f stdout=@filename(t);
  }
}

(sortedfile sfiles[]) qsortAll(textfile fileList, string ifiles[]) {
  foreach f,i in ifiles {
    sfiles[i] = sort(f, fileList);
  }
}

(sortedfile sf) merge2(sortedfile f1, sortedfile f2) {
  app {
    merge2 @filename(f1) @filename(f2) stdout=@filename(sf);
  }
}

(sortedfile mfiles[]) mergeHalf(sortedfile sfiles[]) {
  foreach f,i in sfiles {
    if (i %% 2 == 1) {
      mfiles[i%/2]=merge2(sfiles[i-1], sfiles[i]);
    }
  }
}

(sortedfile mfiles[]) mergeHalfRec(sortedfile sfiles[], int k) {
  if (k > 1) {
    sortedfile tmpfiles[];
    tmpfiles = mergeHalf(sfiles);
    mfiles = mergeHalfRec(tmpfiles, k-1);
  } else {
    mfiles = mergeHalf(sfiles);
  }
}

main (textfile fileList, string strFiles[]) {
    sortedfile s1files[] <simple_mapper;location="./data",prefix="s",suffix=".sorted.4">;
    sortedfile s6files[] <simple_mapper;location="./data",prefix="final",suffix=".data">;

    s1files = qsortAll(fileList, strFiles);

    s6files=mergeHalfRec(s1files, 4);
}

(textfile post_condition) prepareData(string localpath) {
    app {
        split "data.txt" 16 localpath stdout=@post_condition;
    }
}

#string localpath = "/var/tmp/quanpt/swift/scripts/sort/data";
string localpath = "/home/quanpt/assi/swift/sort/data";
string strFiles[] = readData("data/datalist.txt");
textfile fileList;

fileList = prepareData(localpath);
main(fileList, strFiles);

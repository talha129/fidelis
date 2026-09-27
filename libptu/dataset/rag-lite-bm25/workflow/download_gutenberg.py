#!/usr/bin/env python3
"""
Download N English plain-text books from Project Gutenberg.

Usage
-----
  python download_gutenberg.py --output-dir /shared/rag-data
  python download_gutenberg.py --output-dir /shared/rag-data --num-books 500
  python download_gutenberg.py --output-dir /shared/rag-data --delay 1.0
"""

import argparse
import csv
import gzip
import io
import pathlib
import sys
import time
import urllib.request

CATALOG = "https://www.gutenberg.org/cache/epub/feeds/pg_catalog.csv.gz"
MIRROR  = "https://gutenberg.org/cache/epub/{id}/pg{id}.txt"
HEADERS = {"User-Agent": "Mozilla/5.0"}


def fetch_catalog():
    print("Fetching Gutenberg catalog ...")
    req  = urllib.request.Request(CATALOG, headers=HEADERS)
    data = urllib.request.urlopen(req, timeout=60).read()
    rows = list(csv.DictReader(
        io.TextIOWrapper(gzip.open(io.BytesIO(data)), encoding="utf-8")
    ))
    print(f"Catalog entries: {len(rows)}")
    return rows


def filter_candidates(rows):
    return [
        r for r in rows
        if r.get("Language") == "en"
        and r.get("Type", "") == "Text"
        and r.get("Text#", "").isdigit()
    ]


def download_books(candidates, output_dir, num_books, delay):
    output_dir.mkdir(parents=True, exist_ok=True)
    downloaded = 0
    skipped    = 0
    failed     = 0

    for row in candidates:
        if downloaded >= num_books:
            break

        book_id = row["Text#"]
        title   = row.get("Title", "unknown")[:60].replace("/", "_")
        dest    = output_dir / f"pg{book_id}.txt"

        if dest.exists():
            downloaded += 1
            print(f"[{downloaded}/{num_books}] CACHED  {book_id}: {title}")
            continue

        url = MIRROR.format(id=book_id)
        try:
            req  = urllib.request.Request(url, headers=HEADERS)
            body = urllib.request.urlopen(req, timeout=30).read()
            dest.write_bytes(body)
            downloaded += 1
            print(f"[{downloaded}/{num_books}] OK      {book_id}: {title} ({len(body):,} bytes)")
        except Exception as e:
            failed += 1
            skipped += 1
            print(f"  SKIP {book_id}: {e}", file=sys.stderr)

        time.sleep(delay)

    return downloaded, failed


def main():
    parser = argparse.ArgumentParser(description="Download Gutenberg books as .txt files")
    parser.add_argument("--output-dir", required=True,
                        help="Directory to save downloaded .txt files")
    parser.add_argument("--num-books", type=int, default=1000,
                        help="Number of books to download (default: 1000)")
    parser.add_argument("--skip", type=int, default=0,
                        help="Skip first N candidates in catalog (default: 0)")
    parser.add_argument("--delay", type=float, default=0.5,
                        help="Seconds between requests (default: 0.5, min recommended: 0.5)")
    args = parser.parse_args()

    output_dir = pathlib.Path(args.output_dir)

    rows       = fetch_catalog()
    candidates = filter_candidates(rows)
    print(f"English text candidates: {len(candidates)}, skipping {args.skip}, targeting {args.num_books}")
    candidates = candidates[args.skip:]

    downloaded, failed = download_books(candidates, output_dir, args.num_books, args.delay)

    print(f"\nDone: {downloaded} downloaded, {failed} failed")
    print(f"Files in {output_dir}: {len(list(output_dir.glob('*.txt')))}")


if __name__ == "__main__":
    main()

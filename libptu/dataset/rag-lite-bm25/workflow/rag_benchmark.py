#!/usr/bin/env python3
"""
RAG-Lite BM25 benchmark using TaskVine.

Usage
-----
  # Run with default data directory:
  python rag_benchmark.py

  # Custom data directory and output:
  python rag_benchmark.py --data-dir /shared/rag-data/books --output corpus.json

  # Limit number of books:
  python rag_benchmark.py --data-dir /shared/rag-data --num-files 10

  # Custom manager name and ports:
  python rag_benchmark.py --name rag-manager --ports 9123 9150

  # Skip BM25 query phase:
  python rag_benchmark.py --skip-query

  # Enable warm-pool:
  TASKVINE_WARM_POOL=1 python rag_benchmark.py
"""

import argparse
import json
import os
import sys
import time
from collections import Counter
from pathlib import Path

import ndcctools.taskvine as vine


def clean_and_chunk_book(local_filename: str, book_id: str):
    """
    Read a local Gutenberg .txt file staged by TaskVine, clean it,
    and chunk it into ~1000-character segments.

    Returns list of dicts with chunk text and metadata.
    """
    import re
    from langchain_text_splitters import RecursiveCharacterTextSplitter

    with open(local_filename, "r", encoding="utf-8", errors="ignore") as f:
        text = f.read()

    # Strip Gutenberg boilerplate
    for marker in [
        "*** START OF THIS PROJECT GUTENBERG",
        "*** START OF THE PROJECT GUTENBERG",
        "***START OF THE PROJECT GUTENBERG",
        "*END*THE SMALL PRINT",
    ]:
        idx = text.find(marker)
        if idx != -1:
            text = text[idx + len(marker):]
            break

    for marker in [
        "*** END OF THIS PROJECT GUTENBERG",
        "*** END OF THE PROJECT GUTENBERG",
        "***END OF THE PROJECT GUTENBERG",
    ]:
        idx = text.find(marker)
        if idx != -1:
            text = text[:idx]
            break

    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"\n\s*\n\s*\n+", "\n\n", text)
    text = re.sub(r" +", " ", text)
    text = text.strip()

    splitter = RecursiveCharacterTextSplitter(
        chunk_size=1000,
        chunk_overlap=200,
        separators=["\n\n", "\n", ". ", " ", ""],
        length_function=len,
    )
    chunks = splitter.split_text(text)
    total_chunks = len(chunks)

    results = []
    for i, chunk in enumerate(chunks):
        words = re.findall(r"\b\w+\b", chunk)
        relative_position = i / (total_chunks - 1) if total_chunks > 1 else 0.0
        results.append({
            "book_id": book_id,
            "chunk_id": i,
            "total_chunks": total_chunks,
            "relative_position": relative_position,
            "text": chunk,
            "chunk_length": len(chunk),
            "n_chars": len(chunk),
            "n_words": len(words),
            "preview": chunk[:160].replace("\n", " "),
        })

    return results


def run_bm25_queries(corpus, queries, k=4):
    """Build BM25 index from corpus and run queries."""
    from langchain_core.documents import Document
    from langchain_community.retrievers import BM25Retriever

    documents = [
        Document(
            page_content=c["text"],
            metadata={
                "book_id": c["book_id"],
                "chunk_id": c["chunk_id"],
                "relative_position": c["relative_position"],
                "n_words": c["n_words"],
            },
        )
        for c in corpus
    ]

    print(f"Building BM25 index over {len(documents)} chunks ...")
    retriever = BM25Retriever.from_documents(documents)
    retriever.k = k
    print("BM25 index ready.")

    for query in queries:
        results = retriever.invoke(query)
        print("\n" + "=" * 70)
        print(f"Query: {query!r}")
        print("=" * 70)
        for i, doc in enumerate(results, 1):
            meta = doc.metadata
            pos_pct = f"{100 * meta.get('relative_position', 0.0):.1f}%"
            preview = doc.page_content[:200].replace("\n", " ")
            print(f"  [{i}] book={meta['book_id']}  chunk={meta['chunk_id']}  pos={pos_pct}")
            print(f"       {preview}...")


def main():
    parser = argparse.ArgumentParser(description="RAG-Lite BM25 benchmark with TaskVine")
    parser.add_argument("--data-dir", default="/shared/rag-data",
                        help="Directory containing .txt book files (default: /shared/rag-data)")
    parser.add_argument("--output", default="gutenberg_corpus.json",
                        help="Output JSON file for corpus (default: gutenberg_corpus.json)")
    parser.add_argument("--num-files", type=int, default=None,
                        help="Limit number of book files to process from --data-dir (default: all)")
    parser.add_argument("--name", default=os.environ.get("VINE_MANAGER_NAME", "rag-manager"),
                        help="TaskVine manager name (default: $VINE_MANAGER_NAME or 'rag-manager')")
    parser.add_argument("--ports", type=int, nargs="+",
                        default=[int(p.strip()) for p in
                                 os.environ.get("VINE_MANAGER_PORTS", "9123,9150").split(",")],
                        help="TaskVine manager ports (default: $VINE_MANAGER_PORTS or 9123 9150)")
    parser.add_argument("--run-info-path", default="vine-run-info",
                        help="Directory for TaskVine logs (default: vine-run-info)")
    parser.add_argument("--staging-path", default="/tmp/rag-staging",
                        help="Temporary staging directory (default: /tmp/rag-staging)")
    parser.add_argument("--chunk-size", type=int, default=1000,
                        help="Characters per chunk (default: 1000)")
    parser.add_argument("--skip-query", action="store_true",
                        help="Skip BM25 query phase after chunking")
    parser.add_argument("--queries", nargs="+",
                        default=[
                            "What happens when Alice falls down the rabbit hole?",
                            "What does Hamlet mean when he says 'To be or not to be'?",
                        ],
                        help="Queries to run against BM25 index")
    args = parser.parse_args()

    # ── Discover books ────────────────────────────────────────────────────────
    data_dir = Path(args.data_dir)
    if not data_dir.exists():
        print(f"ERROR: data directory {data_dir} not found.")
        sys.exit(1)

    book_paths = sorted(data_dir.glob("*.txt"))
    if not book_paths:
        print(f"ERROR: No .txt files found in {data_dir}")
        sys.exit(1)

    if args.num_files is not None:
        book_paths = book_paths[:args.num_files]

    print(f"Found {len(book_paths)} book(s):")
    for p in book_paths:
        print(f"  {p.name} ({p.stat().st_size:,} bytes)")

    # ── Manager ───────────────────────────────────────────────────────────────
    m = vine.Manager(
        args.ports,
        name=args.name,
        run_info_path=args.run_info_path,
        staging_path=args.staging_path,
    )
    print(f"\nTaskVine manager: name={args.name!r}, ports={args.ports}")

    # ── Phase 1: Submit chunking tasks ────────────────────────────────────────
    print(f"\nPhase 1: Submitting {len(book_paths)} chunking task(s) ...")
    task_meta = {}
    start_time = time.time()

    for path in book_paths:
        book_id = path.stem
        f = m.declare_file(str(path), cache="worker")
        task = vine.PythonTask(clean_and_chunk_book, "book.txt", book_id)
        task.add_input(f, "book.txt")
        task.set_cores(1)
        t_id = m.submit(task)
        task_meta[t_id] = {"book_id": book_id, "path": str(path)}
        print(f"  Submitted task {t_id} for {book_id}")

    # ── Phase 2: Collect results ──────────────────────────────────────────────
    print("\nPhase 2: Waiting for workers ...")
    corpus = []
    completed = 0
    total = len(book_paths)

    while not m.empty():
        t = m.wait(5)
        if not t:
            continue
        completed += 1
        book_id = task_meta[t.id]["book_id"]
        if t.successful():
            chunks = t.output
            corpus.extend(chunks)
            print(f"  [{completed}/{total}] {book_id} -> {len(chunks)} chunks")
        else:
            print(f"  [{completed}/{total}] {book_id} FAILED: {t.result}")

    elapsed = time.time() - start_time
    print(f"\nChunking complete in {elapsed:.2f}s ({elapsed/60:.2f} min)")
    print(f"Total chunks: {len(corpus)}")

    m.workflow_summary(verbose=True)

    # ── Save corpus ───────────────────────────────────────────────────────────
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(corpus, f, ensure_ascii=False, indent=2)
    print(f"Corpus saved to {args.output}")

    # ── Stats ─────────────────────────────────────────────────────────────────
    book_counts = Counter(c["book_id"] for c in corpus)
    print("\nChunks per book:")
    for b, cnt in sorted(book_counts.items()):
        print(f"  {b}: {cnt}")

    chunk_lengths = [c["chunk_length"] for c in corpus]
    if chunk_lengths:
        print(f"\nChunk length: min={min(chunk_lengths)}  max={max(chunk_lengths)}  "
              f"avg={sum(chunk_lengths)/len(chunk_lengths):.1f}")

    # ── Phase 3: BM25 queries ─────────────────────────────────────────────────
    if not args.skip_query and corpus:
        print("\nPhase 3: BM25 retrieval ...")
        run_bm25_queries(corpus, args.queries)


if __name__ == "__main__":
    main()

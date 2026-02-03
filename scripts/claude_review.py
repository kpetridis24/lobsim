#!/usr/bin/env python3
"""
Claude-powered code review for LOBSIM pull requests.

This script:
1. Reads PR diff and changed files
2. Sends code to Claude API for review
3. Posts inline comments and summary to GitHub PR
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import anthropic
import requests

# Configuration
MAX_DIFF_LINES_PER_FILE = 500
CLAUDE_MODEL = "claude-sonnet-4-20250514"
MAX_RETRIES = 3
RETRY_DELAY = 5  # seconds

# File patterns to always skip
SKIP_PATTERNS = [
    r"^build/",
    r"^venv/",
    r"__pycache__",
    r"\.pyc$",
    r"CMakeCache\.txt",
    r"\.cmake$",
]


def get_project_context() -> str:
    """Read CLAUDE.md if it exists for project-specific context."""
    claude_md = Path("CLAUDE.md")
    if claude_md.exists():
        content = claude_md.read_text()
        # Extract key sections for review context (keep it concise)
        sections = []

        # Extract Code Review Focus section
        if "## Code Review Focus" in content:
            start = content.find("## Code Review Focus")
            end = content.find("\n## ", start + 1)
            if end == -1:
                end = len(content)
            sections.append(content[start:end].strip())

        # Extract Code Conventions section
        if "## Code Conventions" in content:
            start = content.find("## Code Conventions")
            end = content.find("\n## ", start + 1)
            if end == -1:
                end = len(content)
            sections.append(content[start:end].strip())

        if sections:
            return "\n\n".join(sections)

    return ""


@dataclass
class InlineComment:
    """Represents a single inline comment on a specific line."""

    path: str
    line: int
    body: str
    side: str = "RIGHT"


@dataclass
class ReviewResult:
    """Complete review result."""

    summary: str
    severity: str = "comment"  # comment, approve, request_changes


@dataclass
class FileDiff:
    """Parsed diff for a single file."""

    path: str
    hunks: list[dict] = field(default_factory=list)


def parse_unified_diff(diff_text: str) -> dict[str, FileDiff]:
    """
    Parse unified diff format into structured FileDiff objects.

    Returns mapping of file path -> FileDiff
    """
    files: dict[str, FileDiff] = {}
    current_file: str | None = None
    current_hunks: list[dict] = []
    current_hunk: dict | None = None
    new_line_num = 0

    for line in diff_text.split("\n"):
        # New file header
        if line.startswith("diff --git"):
            if current_file and current_hunks:
                files[current_file] = FileDiff(path=current_file, hunks=current_hunks)
            # Extract file path (b/path/to/file)
            match = re.search(r" b/(.+)$", line)
            current_file = match.group(1) if match else None
            current_hunks = []
            current_hunk = None

        # Hunk header @@ -old_start,old_count +new_start,new_count @@
        elif line.startswith("@@"):
            if current_hunk:
                current_hunks.append(current_hunk)
            match = re.search(r"\+(\d+)", line)
            new_line_num = int(match.group(1)) if match else 1
            current_hunk = {"start_line": new_line_num, "lines": []}

        # Content lines
        elif current_hunk is not None:
            if line.startswith("+") and not line.startswith("+++"):
                current_hunk["lines"].append(
                    {"type": "add", "line_num": new_line_num, "content": line[1:]}
                )
                new_line_num += 1
            elif line.startswith("-") and not line.startswith("---"):
                current_hunk["lines"].append(
                    {"type": "delete", "line_num": None, "content": line[1:]}
                )
            elif line.startswith(" "):
                current_hunk["lines"].append(
                    {"type": "context", "line_num": new_line_num, "content": line[1:]}
                )
                new_line_num += 1

    # Don't forget last file/hunk
    if current_hunk:
        current_hunks.append(current_hunk)
    if current_file and current_hunks:
        files[current_file] = FileDiff(path=current_file, hunks=current_hunks)

    return files


def build_review_prompt(file_diff: FileDiff, project_context: str = "") -> str:
    """Build a detailed prompt for Claude to review the code."""
    ext = Path(file_diff.path).suffix
    is_cpp = ext in [".cpp", ".hpp", ".h", ".cc", ".c"]
    is_python = ext == ".py"

    language_context = ""
    if is_cpp:
        language_context = """
This is a C++20 codebase for a high-performance limit order book simulator (LOBSIM).
Key conventions:
- LLVM code style with 4-space indent, 120 char line limit
- Uses Boost intrusive containers for performance
- Custom FlatHashMap for hot paths
- Heavy use of templates and constexpr
- Performance-critical code paths marked with LOBSIM_FORCEINLINE
- Memory managed via NodePool object pools
"""
    elif is_python:
        language_context = """
This is Python code for the lobsim package (bindings to C++ core).
Key conventions:
- Python 3.11+ type hints required
- Ruff for formatting
- Wrapper classes around C++ _core module
"""

    # Add project-specific context from CLAUDE.md if available
    if project_context:
        language_context += f"\n## Project-Specific Guidelines (from CLAUDE.md):\n{project_context}\n"

    # Build hunk content
    hunk_text = ""
    line_count = 0
    for i, hunk in enumerate(file_diff.hunks):
        hunk_text += f"\n--- Hunk {i + 1} (starting line {hunk['start_line']}) ---\n"
        for line_info in hunk["lines"]:
            if line_count >= MAX_DIFF_LINES_PER_FILE:
                hunk_text += "\n... (truncated, too many lines) ...\n"
                break
            prefix = (
                "+"
                if line_info["type"] == "add"
                else ("-" if line_info["type"] == "delete" else " ")
            )
            line_num = line_info["line_num"] or ""
            hunk_text += f"{prefix} L{line_num}: {line_info['content']}\n"
            line_count += 1

    prompt = f"""You are an expert code reviewer for a high-frequency trading simulation library (LOBSIM).
Review the following code changes and provide actionable feedback.

{language_context}

## File: {file_diff.path}

## Changes:
{hunk_text}

## Review Focus Areas:
1. **Logic**: Correctness, edge cases, off-by-one errors, algorithm soundness
2. **Style**: Consistency with codebase conventions (see above)
3. **Performance**: Hot paths, unnecessary allocations, cache efficiency, lock contention
4. **Security**: Buffer overflows, integer overflow, unsafe pointer operations
5. **Documentation**: Missing comments for complex logic

## Response Format:
Respond with a JSON object:
{{
  "summary": "Brief overall assessment (1-2 sentences)",
  "severity": "approve|comment|request_changes",
  "inline_comments": [
    {{
      "line": <line_number_in_new_file>,
      "category": "logic|style|performance|security|documentation",
      "severity": "info|warning|error",
      "message": "Your comment here"
    }}
  ]
}}

Rules:
- Only comment on NEW or MODIFIED lines (+ lines), not deleted lines
- Be specific and actionable
- For style issues, reference the convention being violated
- For performance, explain WHY something is slow
- Limit to 5-10 most important issues per file
- If code looks good, say so briefly with "severity": "approve"
- Return valid JSON only, no markdown code blocks
"""
    return prompt


def call_claude_api(prompt: str, retries: int = MAX_RETRIES) -> dict[str, Any]:
    """Call Claude API with retry logic and rate limit handling."""
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        print("Error: ANTHROPIC_API_KEY not set")
        return {"summary": "API key not configured", "inline_comments": []}

    client = anthropic.Anthropic(api_key=api_key)

    for attempt in range(retries):
        try:
            response = client.messages.create(
                model=CLAUDE_MODEL,
                max_tokens=4096,
                messages=[{"role": "user", "content": prompt}],
                system="You are a senior software engineer reviewing code. Respond only with valid JSON, no markdown formatting.",
            )

            # Extract text content
            text = response.content[0].text

            # Parse JSON response (handle markdown code blocks if present)
            if text.startswith("```"):
                text = re.sub(r"^```(?:json)?\n?", "", text)
                text = re.sub(r"\n?```$", "", text)

            return json.loads(text)

        except anthropic.RateLimitError:
            wait_time = RETRY_DELAY * (attempt + 1)
            print(f"Rate limited, waiting {wait_time}s...")
            time.sleep(wait_time)

        except anthropic.APIError as e:
            print(f"API error: {e}")
            if attempt == retries - 1:
                return {"summary": f"API error: {e}", "inline_comments": []}
            time.sleep(RETRY_DELAY)

        except json.JSONDecodeError as e:
            print(f"Failed to parse JSON response: {e}")
            if attempt == retries - 1:
                return {
                    "summary": "Failed to parse review response",
                    "inline_comments": [],
                }
            time.sleep(RETRY_DELAY)

    return {"summary": "Review failed after retries", "inline_comments": []}


def post_review_to_github(
    review_result: ReviewResult,
    file_comments: dict[str, list[InlineComment]],
) -> None:
    """Post review comments to GitHub PR using the Pull Request Review API."""
    token = os.environ.get("GITHUB_TOKEN")
    repo = os.environ.get("REPO_FULL_NAME")
    pr_number = os.environ.get("PR_NUMBER")
    commit_sha = os.environ.get("PR_HEAD_SHA")

    if not all([token, repo, pr_number, commit_sha]):
        print("Error: Missing required environment variables")
        print(f"  GITHUB_TOKEN: {'set' if token else 'missing'}")
        print(f"  REPO_FULL_NAME: {repo or 'missing'}")
        print(f"  PR_NUMBER: {pr_number or 'missing'}")
        print(f"  PR_HEAD_SHA: {commit_sha or 'missing'}")
        return

    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github.v3+json",
    }

    # Build review comments array
    comments = []
    for path, path_comments in file_comments.items():
        for comment in path_comments:
            comments.append(
                {
                    "path": path,
                    "line": comment.line,
                    "side": comment.side,
                    "body": comment.body,
                }
            )

    # Map severity to GitHub review event
    event_map = {
        "approve": "APPROVE",
        "request_changes": "REQUEST_CHANGES",
        "comment": "COMMENT",
    }
    event = event_map.get(review_result.severity, "COMMENT")

    # Build review body
    review_body = f"""## Claude Code Review Summary

{review_result.summary}

---
*Review focus: logic, style, performance, security, documentation*
*Automated review powered by Claude*
"""

    payload: dict[str, Any] = {
        "commit_id": commit_sha,
        "body": review_body,
        "event": event,
    }

    # Only add comments if we have any
    if comments:
        payload["comments"] = comments

    url = f"https://api.github.com/repos/{repo}/pulls/{pr_number}/reviews"

    response = requests.post(url, headers=headers, json=payload, timeout=30)

    if response.status_code == 201:
        print(f"Review posted successfully: {response.json().get('html_url')}")
    else:
        print(f"Failed to post review: {response.status_code}")
        print(response.json())


def main() -> None:
    """Main entry point for the review script."""
    # Read diff and changed files
    diff_path = Path("pr_diff.patch")
    files_path = Path("changed_files.txt")

    if not diff_path.exists():
        print("No diff file found")
        sys.exit(0)

    diff_text = diff_path.read_text()

    if not files_path.exists() or files_path.stat().st_size == 0:
        print("No reviewable files changed")
        sys.exit(0)

    changed_files = [f.strip() for f in files_path.read_text().split("\n") if f.strip()]

    # Parse diff
    file_diffs = parse_unified_diff(diff_text)

    # Filter to only changed files we care about
    relevant_diffs = {
        path: diff
        for path, diff in file_diffs.items()
        if path in changed_files
        and not any(re.search(p, path) for p in SKIP_PATTERNS)
    }

    if not relevant_diffs:
        print("No relevant changes to review")
        sys.exit(0)

    print(f"Reviewing {len(relevant_diffs)} files...")

    # Load project context from CLAUDE.md (once for all files)
    project_context = get_project_context()
    if project_context:
        print("  Loaded project context from CLAUDE.md")

    # Review each file
    all_comments: dict[str, list[InlineComment]] = {}
    file_summaries = []
    overall_severity = "approve"

    for path, file_diff in relevant_diffs.items():
        print(f"  Reviewing: {path}")

        # Build prompt and call Claude
        prompt = build_review_prompt(file_diff, project_context)
        result = call_claude_api(prompt)

        # Process inline comments
        inline_comments: list[InlineComment] = []
        for comment in result.get("inline_comments", []):
            # Format the comment with category badge
            category = comment.get("category", "general")
            severity = comment.get("severity", "info")
            emoji_map = {"error": ":x:", "warning": ":warning:", "info": ":bulb:"}
            emoji = emoji_map.get(severity, ":speech_balloon:")

            body = f"**[{category.upper()}]** {emoji} {comment['message']}"

            inline_comments.append(
                InlineComment(path=path, line=comment["line"], body=body)
            )

        if inline_comments:
            all_comments[path] = inline_comments

        # Track summary
        file_summaries.append(f"- **{path}**: {result.get('summary', 'No issues')}")

        # Escalate severity
        if result.get("severity") == "request_changes":
            overall_severity = "request_changes"
        elif result.get("severity") == "comment" and overall_severity == "approve":
            overall_severity = "comment"

    # Build overall summary
    overall_summary = "### Files Reviewed:\n" + "\n".join(file_summaries)

    if overall_severity == "approve":
        overall_summary = (
            ":white_check_mark: **Looks good!** No major issues found.\n\n"
            + overall_summary
        )
    elif overall_severity == "request_changes":
        overall_summary = (
            ":warning: **Changes requested** - please address the comments below.\n\n"
            + overall_summary
        )

    review_result = ReviewResult(summary=overall_summary, severity=overall_severity)

    # Post to GitHub
    post_review_to_github(review_result, all_comments)

    print("Review complete!")


if __name__ == "__main__":
    main()

<#
    Declared release inputs.

    This file is the answer to one question: what does producing a SpoutPlugin release actually
    require from this repository?

    New-CleanSourceCopy.ps1 builds a clean copy containing ONLY what is declared here and runs the
    whole release from it. If the release succeeds, nothing undeclared was needed. If it fails, the
    missing input is named rather than silently supplied by the author's working directory, which
    is how a release ends up working on exactly one machine.

    Each entry carries its git-tracking expectation. The clean-source report compares that against
    read-only `git ls-files` and flags every declared input that is NOT tracked, because an
    untracked input means a fresh clone cannot reproduce the release even though a clean copy of
    this working tree can. Those are two different claims and are reported separately.

    Patterns are repository-relative and use forward slashes. '...' means "this directory and
    everything under it".
#>
@{
    SchemaVersion = 1

    # Everything the plugin release archive is built from. Without any one of these,
    # RunUAT BuildPlugin either fails or produces an archive that fails validation.
    Required = @(
        @{ Path = 'SpoutPlugin.uplugin';         Category = 'descriptor'; Note = 'Release version is read from this file; nothing hard-codes it.' }
        @{ Path = 'Source/...';                  Category = 'source';     Note = 'The runtime module, including the automation tests.' }
        @{ Path = 'ThirdParty/...';              Category = 'thirdparty'; Note = 'Spout headers, import library and runtime DLL.' }
        @{ Path = 'Config/FilterPlugin.ini';     Category = 'packaging';  Note = 'Tells BuildPlugin to ship ThirdParty and the licensing files.' }
        @{ Path = 'Content/...';                 Category = 'content';    Note = 'Example Blueprints and assets.' }
        @{ Path = 'Resources/...';               Category = 'content';    Note = 'Plugin icon, required by the descriptor.' }
        @{ Path = 'LICENSE';                     Category = 'licensing';  Note = 'AUD-011. build.ps1 fails the release when this is absent from the repo or any ZIP.' }
        @{ Path = 'THIRD_PARTY_NOTICES.md';      Category = 'licensing';  Note = 'AUD-011. Consolidated Spout/Spout2 BSD notice and binary provenance.' }
        @{ Path = 'README.md';                   Category = 'docs';       Note = 'Shipped inside the archive so an unzipped plugin is self-describing.' }
        @{ Path = 'CHANGELOG.md';                Category = 'docs';       Note = 'Technical source of truth for what changed; validated for version agreement.' }
        @{ Path = 'build.ps1';                   Category = 'release';    Note = 'AUD-010. The release script itself is a release input.' }
        @{ Path = 'build.bat';                   Category = 'release';    Note = 'AUD-010. Propagates the PowerShell exit code to the caller.' }
        @{ Path = 'Scripts/...';                 Category = 'release';    Note = 'Track D validation pipeline, including this manifest.' }
    )

    # Needed to VALIDATE a release rather than to produce one. A clean copy without these can still
    # build and validate archives; it cannot run the packaged-host matrix.
    Validation = @(
        @{ Path = 'Tests/...';                   Category = 'validation'; Note = 'Host test project used by the packaged-host matrix and the performance baselines.' }
        @{ Path = 'Docs/...';                    Category = 'docs';       Note = 'Audit, track reports and the documentation set the drift check reads.' }
    )

    # Present in the working tree and deliberately NOT copied into a clean release copy. Listed so
    # the exclusion is a decision on the record rather than an accident of the copy logic.
    Excluded = @(
        @{ Path = 'Artifacts/...';   Reason = 'Generated validation evidence. Must never reach a release archive.' }
        @{ Path = 'Binaries/...';    Reason = 'Build output.' }
        @{ Path = 'Intermediate/...';Reason = 'Build output. Note that BuildPlugin PRODUCES an Intermediate/Build tree in its package, which IS shipped; that is a different directory from this one.' }
        @{ Path = 'Saved/...';       Reason = 'Editor scratch.' }
        @{ Path = '.git/...';        Reason = 'Version-control metadata. The clean copy is built by file copy, never by a git checkout.' }
        @{ Path = '.vs/...';         Reason = 'IDE scratch.' }
        @{ Path = '.vscode/...';     Reason = 'IDE settings; not a release input.' }
        @{ Path = 'CLAUDE.md';       Reason = 'Repository working instructions; not shipped.' }
        @{ Path = 'TRACK_A_*.md';    Reason = 'Track implementation records; internal.' }
        @{ Path = 'UPDATES_NOTES.md';Reason = 'Public-facing release notes, published separately from the plugin archive.' }
    )
}

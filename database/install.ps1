param(
  [string] $DatabaseName = $(if ($env:DATABASE_NAME) { $env:DATABASE_NAME } else { "message_store.sqlite3" }),
  [string] $Extension = $(if ($env:MSGDB_EXTENSION) { $env:MSGDB_EXTENSION } else { "build/extension/message_db.dll" }),
  [string] $Sqlite = $(if ($env:SQLITE) { $env:SQLITE } else { "sqlite3" })
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$extensionPath = Resolve-Path $Extension
$databasePath = $DatabaseName

$script = @"
.bail on
.load '$extensionPath'
.read '$Root/database/schema/message-store.sql'
.read '$Root/database/tables/messages.sql'
.read '$Root/database/indexes/messages-id.sql'
.read '$Root/database/indexes/messages-stream.sql'
.read '$Root/database/indexes/messages-category.sql'
.read '$Root/database/views/category-type-summary.sql'
.read '$Root/database/views/stream-summary.sql'
.read '$Root/database/views/stream-type-summary.sql'
.read '$Root/database/views/type-category-summary.sql'
.read '$Root/database/views/type-stream-summary.sql'
.read '$Root/database/views/type-summary.sql'
"@

$script | & $Sqlite $databasePath

if ($LASTEXITCODE -ne 0) {
  throw "sqlite3 failed with exit code $LASTEXITCODE"
}

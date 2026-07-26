# Launching thorstream from Cocoon

Adds your PC's Playnite library to [Cocoon](https://cocoon-shell.com/) as a
platform, so picking a game there starts it on the PC and streams it back.

## Files

- `thorstream.json` — the platform definition to import into Cocoon.
- `thorstream/` — one `.thor` tag file per game, generated from the Playnite
  library. The filename is the game's name, which is the only part that matters.

## Setup

1. Copy the `thorstream/` folder onto the Thor, anywhere Cocoon scans for ROMs.
2. Copy `thorstream.json` onto the Thor as well.
3. In Cocoon, import `thorstream.json` as a platform, and point the new
   "Windows PC (thorstream)" platform at the `thorstream/` folder.
4. Scrape artwork as usual, or let it use the covers you set in the host's web
   console — those only reach the handheld through the thorstream app itself,
   so Cocoon needs its own scrape.

## How it works

Cocoon launches the client with the tag file's path as a string extra:

```
am start -n com.thorstream.client/.MainActivity --es path <file>
```

The client takes the base name of that path as the game's name, finds the host
(the address it used last, or a broadcast scan if that fails), looks the name up
in the live Playnite library, and starts streaming it. Nothing is hard-coded to
a machine or a game id, so entries keep working when the PC changes address and
when Playnite re-imports a game under a new id.

Name matching is exact first, then ignoring everything except letters and
digits — Windows filenames cannot contain a colon or a question mark, so
`Marvel's Spider-Man Remastered` still matches whatever punctuation Playnite
holds.

## Regenerating the tag files

After adding games to Playnite, from the repository root:

```powershell
$tsv = & tools\PlayniteLibrary\bin\Release\net10.0\PlayniteLibrary.exe --tsv
New-Item -ItemType Directory -Force -Path cocoon\thorstream | Out-Null
foreach ($line in $tsv) {
  $name = ($line -split "`t")[1]
  if (-not $name) { continue }
  $safe = ($name -replace '[\\/:*?"<>|]', ' ').Trim()
  Set-Content "cocoon\thorstream\$safe.thor" "[game] $name" -Encoding utf8
}
```

## If a game does not start

The client says `"<name>" is not in the Playnite library` when it connected but
found no match — rename the `.thor` file to match Playnite exactly. If it never
gets that far it will say it could not find the PC, which is the host not
running rather than anything to do with Cocoon.

## A caveat on the format

Cocoon's platform JSON is Daijishō-compatible, but Cocoon does not publish a
schema, and in particular it does not document the token used to substitute tag
file contents into launcher arguments. This definition therefore avoids that
entirely: it passes only `{file.path}`, which is the long-standing Daijishō
token, and does the parsing in the client. If your Cocoon build names that token
differently, `amStartArguments` in `thorstream.json` is the only line to change.

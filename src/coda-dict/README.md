# CODA middle tag dictionary

`tags-v1.txt` is the static tag dictionary of the CODA middle. The line number
(starting at 1) is the id of an entry:

* `K<TAB>key`: a key, stored as its id followed by the value;
* `T<TAB>key<TAB>value`: a complete tag, stored as its id only.

Tabs, newlines and backslashes in keys and values are escaped with a backslash.

It combines the frequent keys (7,128 keys used at least 1,000 times) and the
most frequent tags (24,872 tags, filling the ids up to 32,000) from Jochen
Topf's osm2pgsql-middle-experiments (`stats/frequent-keys.json` and
`stats/frequent-tags.json`, GPL-2.0), computed from the planet.

`tags-v1.zdict` is a zstd dictionary (112 KB) trained on the encoded tags of a
0.3% sample of the planet's ways.

Both files are compiled into osm2pgsql and copied into every CODA database
when it is created, so existing databases keep working if a later version of
osm2pgsql comes with a different dictionary.

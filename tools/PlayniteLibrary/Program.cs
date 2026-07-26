using System.Text.Json;
using LiteDB;

// Reads a Playnite library and prints the games it contains.
//
// Playnite ships LiteDB 4.1.4 and its database is in that format; the LiteDB 5
// package cannot open it. The file is always copied before opening, so the live
// library is never touched even if Playnite is running.
namespace PlayniteLibrary;

internal record GameEntry(
    string Id,
    string Name,
    string Platform,
    string Source,
    bool Installed,
    string InstallDirectory,
    string Action,
    string CoverImage);

internal static class Program
{
    private static int Main(string[] args)
    {
        string libraryDir = args.FirstOrDefault(a => !a.StartsWith("--"))
            ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                            "Playnite", "library");
        bool json = args.Contains("--json");
        bool installedOnly = args.Contains("--installed");

        string gamesDb = Path.Combine(libraryDir, "games.db");
        if (!File.Exists(gamesDb))
        {
            Console.Error.WriteLine($"No Playnite library at {gamesDb}");
            return 1;
        }

        // Work on a snapshot: opening the live file risks disturbing a running
        // Playnite, and this tool has no business writing to someone's library.
        string temp = Path.Combine(Path.GetTempPath(), "thorstream-playnite");
        Directory.CreateDirectory(temp);
        var lookups = new Dictionary<string, Dictionary<string, string>>();

        foreach (var name in new[] { "games", "platforms", "sources" })
        {
            string source = Path.Combine(libraryDir, $"{name}.db");
            if (File.Exists(source)) File.Copy(source, Path.Combine(temp, $"{name}.db"), true);
        }

        // Collection names are singular and capitalised, and do not match the
        // file names: games.db holds "Game", sources.db holds "GameSource".
        lookups["platforms"] = ReadNameLookup(Path.Combine(temp, "platforms.db"), "Platform");
        lookups["sources"] = ReadNameLookup(Path.Combine(temp, "sources.db"), "GameSource");

        if (args.Contains("--collections"))
        {
            foreach (var name in new[] { "games", "platforms", "sources" })
            {
                string file = Path.Combine(temp, $"{name}.db");
                if (!File.Exists(file)) continue;
                using var probe = new LiteDatabase($"Filename={file};Journal=false");
                Console.WriteLine($"{name}.db collections: {string.Join(", ", probe.GetCollectionNames())}");
                foreach (var collection in probe.GetCollectionNames())
                {
                    var first = probe.GetCollection(collection).FindAll().FirstOrDefault();
                    Console.WriteLine($"  {collection}: {probe.GetCollection(collection).Count()} docs");
                    if (first != null)
                        Console.WriteLine($"    fields: {string.Join(", ", first.Keys)}");
                }
            }
            return 0;
        }

        var games = ReadGames(Path.Combine(temp, "games.db"),
                              lookups.GetValueOrDefault("platforms") ?? new(),
                              lookups.GetValueOrDefault("sources") ?? new());

        if (installedOnly) games = games.Where(g => g.Installed).ToList();
        games = games.OrderBy(g => g.Name, StringComparer.OrdinalIgnoreCase).ToList();

        if (json)
        {
            Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(games,
                new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }

        // Tab-separated for the host to consume. Deliberately not JSON: the host
        // is C++ and this saves it a parser dependency for six fixed fields.
        // Tabs and newlines are stripped from values so a row is always a row.
        if (args.Contains("--tsv"))
        {
            foreach (var game in games)
            {
                Console.WriteLine(string.Join('\t',
                    Clean(game.Id), Clean(game.Name), Clean(game.Platform), Clean(game.Source),
                    game.Installed ? "1" : "0", Clean(game.InstallDirectory), Clean(game.CoverImage)));
            }
            return 0;
        }

        Console.WriteLine($"{games.Count} games ({games.Count(g => g.Installed)} installed)\n");
        foreach (var game in games)
        {
            Console.WriteLine($"  {(game.Installed ? "[installed]" : "[         ]")} " +
                              $"{Truncate(game.Name, 44),-44} {game.Platform,-18} {game.Source}");
        }
        return 0;
    }

    private static string Clean(string value) =>
        string.IsNullOrEmpty(value) ? "" : value.Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ');

    private static string Truncate(string value, int max) =>
        string.IsNullOrEmpty(value) ? "" : value.Length <= max ? value : value[..(max - 1)] + "…";

    private static Dictionary<string, string> ReadNameLookup(string path, string collection)
    {
        var result = new Dictionary<string, string>();
        if (!File.Exists(path)) return result;

        using var db = new LiteDatabase($"Filename={path};Journal=false");
        foreach (var doc in db.GetCollection(collection).FindAll())
        {
            var id = doc["_id"].AsGuid.ToString();
            result[id] = doc.ContainsKey("Name") ? doc["Name"].AsString ?? "" : "";
        }
        return result;
    }

    private static List<GameEntry> ReadGames(string path, Dictionary<string, string> platforms,
                                             Dictionary<string, string> sources)
    {
        var result = new List<GameEntry>();
        using var db = new LiteDatabase($"Filename={path};Journal=false");

        foreach (var doc in db.GetCollection("Game").FindAll())
        {
            result.Add(new GameEntry(
                Id: doc["_id"].AsGuid.ToString(),
                Name: Str(doc, "Name"),
                Platform: LookupFirstOf(doc, "PlatformIds", platforms),
                Source: Lookup(doc, "SourceId", sources),
                Installed: doc.ContainsKey("IsInstalled") && doc["IsInstalled"].AsBoolean,
                InstallDirectory: Str(doc, "InstallDirectory"),
                Action: DescribeAction(doc),
                CoverImage: Str(doc, "CoverImage")));
        }
        return result;
    }

    // A game can be listed under several platforms; the first is the one
    // Playnite shows.
    private static string LookupFirstOf(BsonDocument doc, string key,
                                        Dictionary<string, string> table)
    {
        if (!doc.ContainsKey(key) || doc[key].IsNull || !doc[key].IsArray) return "";
        foreach (var value in doc[key].AsArray)
        {
            var name = table.GetValueOrDefault(value.AsGuid.ToString(), "");
            if (!string.IsNullOrEmpty(name)) return name;
        }
        return "";
    }

    private static string Str(BsonDocument doc, string key) =>
        doc.ContainsKey(key) && !doc[key].IsNull ? doc[key].AsString ?? "" : "";

    private static string Lookup(BsonDocument doc, string key, Dictionary<string, string> table)
    {
        if (!doc.ContainsKey(key) || doc[key].IsNull) return "";
        var id = doc[key].AsGuid.ToString();
        return table.GetValueOrDefault(id, "");
    }

    /// <summary>
    /// How Playnite would launch this game. Modern Playnite stores a list of
    /// GameActions rather than a single PlayAction; the play action is the one
    /// flagged as such, falling back to the first entry.
    /// </summary>
    private static string DescribeAction(BsonDocument doc)
    {
        if (!doc.ContainsKey("GameActions") || doc["GameActions"].IsNull ||
            !doc["GameActions"].IsArray) {
            return "";
        }

        BsonDocument chosen = null;
        foreach (var value in doc["GameActions"].AsArray)
        {
            if (!value.IsDocument) continue;
            var action = value.AsDocument;
            chosen ??= action;
            if (action.ContainsKey("IsPlayAction") && action["IsPlayAction"].AsBoolean)
            {
                chosen = action;
                break;
            }
        }
        if (chosen == null) return "";

        var path = chosen.ContainsKey("Path") ? chosen["Path"].AsString : "";
        var args = chosen.ContainsKey("Arguments") ? chosen["Arguments"].AsString : "";
        return string.IsNullOrEmpty(args) ? path ?? "" : $"{path} {args}";
    }
}

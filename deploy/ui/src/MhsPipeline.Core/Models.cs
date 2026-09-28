// 数据契约模型：与引擎侧 json 一一对应（stream_state / queue_state / progress /
// pipeline.status / identity_assets）。
//
// 纪律：字段名必须与 json 完全一致（JsonPropertyName），任何一侧改动都要同步 ——
// 测试用真实样本做契约比对，防止漂移（ContractTests）。

using System.Text.Json.Serialization;

namespace MhsPipeline.Core;

public sealed class PipelineSnapshot
{
    [JsonPropertyName("state")] public string State { get; set; } = "";
    [JsonPropertyName("delivered")] public int Delivered { get; set; }
    [JsonPropertyName("synced")] public int Synced { get; set; }
    [JsonPropertyName("failed")] public int Failed { get; set; }
    [JsonPropertyName("queued")] public int Queued { get; set; }
    [JsonPropertyName("pending_binding")] public int PendingBinding { get; set; }
    [JsonPropertyName("stage_counts")] public List<int> StageCounts { get; set; } = new();
    [JsonPropertyName("identity_groups")] public List<IdentityGroup> IdentityGroups { get; set; } = new();
    [JsonPropertyName("events")] public List<EventItem> Events { get; set; } = new();
    [JsonPropertyName("eta_seconds")] public double? EtaSeconds { get; set; }
    [JsonPropertyName("current")] public CurrentTask? Current { get; set; }
    [JsonPropertyName("tasks_detail")] public TasksDetail? TasksDetail { get; set; }
    [JsonPropertyName("identity")] public IdentityInfo? Identity { get; set; }
    [JsonPropertyName("import_estimate_seconds")] public double? ImportEstimateSeconds { get; set; }
    [JsonPropertyName("updated_at")] public string UpdatedAt { get; set; } = "";
}

public sealed class IdentityGroup
{
    [JsonPropertyName("dir")] public string Dir { get; set; } = "";
    [JsonPropertyName("id")] public string Id { get; set; } = "";
    [JsonPropertyName("until")] public string Until { get; set; } = "";
    [JsonPropertyName("now")] public bool Now { get; set; }
    [JsonPropertyName("total")] public int Total { get; set; }
    [JsonPropertyName("done")] public int Done { get; set; }
    [JsonPropertyName("failed")] public int Failed { get; set; }
    [JsonPropertyName("pending")] public int Pending { get; set; }
}

public sealed class EventItem
{
    [JsonPropertyName("t")] public string T { get; set; } = "";
    [JsonPropertyName("lv")] public string Lv { get; set; } = "";
    [JsonPropertyName("tx")] public string Tx { get; set; } = "";
    [JsonPropertyName("grp")] public string Grp { get; set; } = "";
}

public sealed class CurrentTask
{
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("status")] public string Status { get; set; } = "";
}

public sealed class TasksDetail
{
    [JsonPropertyName("loading")] public List<LoadingRow> Loading { get; set; } = new();
    [JsonPropertyName("active")] public List<ActiveRow> Active { get; set; } = new();
    [JsonPropertyName("done")] public List<DoneRow> Done { get; set; } = new();
    [JsonPropertyName("failed")] public List<FailedRow> Failed { get; set; } = new();
    [JsonPropertyName("imported")] public List<ImportedRow> Imported { get; set; } = new();
}

public sealed class LoadingRow
{
    [JsonPropertyName("key")] public string Key { get; set; } = "";
    [JsonPropertyName("group")] public string Group { get; set; } = "";
    [JsonPropertyName("note")] public string Note { get; set; } = "";
    [JsonPropertyName("ready")] public bool Ready { get; set; }
}

public sealed class ActiveRow
{
    [JsonPropertyName("key")] public string Key { get; set; } = "";
    [JsonPropertyName("group")] public string Group { get; set; } = "";
    [JsonPropertyName("kind")] public string Kind { get; set; } = "";
    [JsonPropertyName("status")] public string Status { get; set; } = "";
}

public sealed class DoneRow
{
    [JsonPropertyName("key")] public string Key { get; set; } = "";
    [JsonPropertyName("group")] public string Group { get; set; } = "";
    [JsonPropertyName("duration")] public double? Duration { get; set; }
    [JsonPropertyName("frames")] public int? Frames { get; set; }
}

public sealed class FailedRow
{
    [JsonPropertyName("key")] public string Key { get; set; } = "";
    [JsonPropertyName("group")] public string Group { get; set; } = "";
    [JsonPropertyName("error")] public string Error { get; set; } = "";
}

public sealed class ImportedRow
{
    [JsonPropertyName("key")] public string Key { get; set; } = "";
    [JsonPropertyName("group")] public string Group { get; set; } = "";
    [JsonPropertyName("note")] public string Note { get; set; } = "";
}

public sealed class IdentityInfo
{
    [JsonPropertyName("group")] public string Group { get; set; } = "";
    [JsonPropertyName("asset")] public string Asset { get; set; } = "";
    [JsonPropertyName("state")] public string State { get; set; } = "";
    [JsonPropertyName("detail")] public string Detail { get; set; } = "";
    [JsonPropertyName("source")] public string Source { get; set; } = "";
    [JsonPropertyName("suggested_group")] public string SuggestedGroup { get; set; } = "";
    /// <summary>已绑定的全部分组（多演员场景：身份带要能显示"已绑定 N 组"）。</summary>
    [JsonPropertyName("bound")] public Dictionary<string, string>? Bound { get; set; }
    [JsonPropertyName("bound_count")] public int BoundCount { get; set; }
}

public sealed class QueueSnapshot
{
    [JsonPropertyName("updated")] public string Updated { get; set; } = "";
    [JsonPropertyName("tasks")] public List<QueueTask> Tasks { get; set; } = new();
}

public sealed class QueueTask
{
    [JsonPropertyName("key")] public string Key { get; set; } = "";
    [JsonPropertyName("dir")] public string Dir { get; set; } = "";
    [JsonPropertyName("kind")] public string Kind { get; set; } = "";
    [JsonPropertyName("group")] public string Group { get; set; } = "";
    [JsonPropertyName("identity")] public string Identity { get; set; } = "";
    [JsonPropertyName("status")] public string Status { get; set; } = "";
    [JsonPropertyName("error")] public string Error { get; set; } = "";
    [JsonPropertyName("duration")] public double? Duration { get; set; }
    [JsonPropertyName("frames")] public int? Frames { get; set; }
}

public sealed class ProgressSnapshot
{
    [JsonPropertyName("state")] public string State { get; set; } = "";
    [JsonPropertyName("percent")] public double Percent { get; set; }
    [JsonPropertyName("asset_name")] public string AssetName { get; set; } = "";
    [JsonPropertyName("elapsed_seconds")] public double ElapsedSeconds { get; set; }
    [JsonPropertyName("eta_seconds")] public double EtaSeconds { get; set; }
    [JsonPropertyName("pass")] public int Pass { get; set; }
    [JsonPropertyName("pass_count")] public int PassCount { get; set; }
    [JsonPropertyName("current_frame")] public int CurrentFrame { get; set; }
    [JsonPropertyName("total_frames")] public int TotalFrames { get; set; }
    [JsonPropertyName("updated_at")] public string UpdatedAt { get; set; } = "";
}

public sealed class EngineStatus
{
    [JsonPropertyName("instance")] public string Instance { get; set; } = "";
    [JsonPropertyName("pid")] public int? Pid { get; set; }
    [JsonPropertyName("state")] public string State { get; set; } = "";
    [JsonPropertyName("action")] public string Action { get; set; } = "";
    [JsonPropertyName("attempts")] public int Attempts { get; set; }
    [JsonPropertyName("restarts")] public int Restarts { get; set; }
    [JsonPropertyName("updated_at")] public string UpdatedAt { get; set; } = "";
}

public sealed class IdentityIndex
{
    [JsonPropertyName("generated_at")] public string GeneratedAt { get; set; } = "";
    [JsonPropertyName("assets")] public List<IdentityAsset> Assets { get; set; } = new();
    [JsonPropertyName("dirs")] public List<string> Dirs { get; set; } = new();
}

public sealed class IdentityAsset
{
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("path")] public string Path { get; set; } = "";
    [JsonPropertyName("dir")] public string Dir { get; set; } = "";
    [JsonPropertyName("modified")] public string Modified { get; set; } = "";
    [JsonPropertyName("size_kb")] public long SizeKb { get; set; }
}

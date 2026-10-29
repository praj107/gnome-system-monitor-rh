// SPDX-License-Identifier: GPL-2.0-or-later
//! Workload classes: who is responsible for a cgroup or a process, by an
//! ordered table of rules (first match wins), as a WLM classification policy
//! orders its rules.
//!
//! Two passes, both pure functions of what the walk already read (no syscall):
//!  - the path pass (`classifyPath`) looks at a cgroup path alone, using the
//!    naming that systemd writes: the slice layout of systemd.special(7), the
//!    desktop-environment naming `app[-LAUNCHER]-APPID-RANDOM.scope` and
//!    `app[-LAUNCHER]-APPID[@RANDOM].service`, and logind's `session-ID.scope`
//!    (systemd.io/DESKTOP_ENVIRONMENTS). Unit, session and owner come from
//!    `unit_path.zig`, which follows systemd's cgroup-util.c. The names of
//!    containers' cgroups follow the managers' own conventions (docker,
//!    libpod, kubepods, lxc);
//!  - the process pass (`classifyProcesses`) adds what a path cannot say:
//!    kernel threads (PF_KTHREAD, proc_pid_stat(5) flags), pid 1, a process
//!    whose cgroup is unknown (a uid and a controlling tty stand in), and the
//!    ancestry overlay that finds a coding agent's descendants inside a cgroup
//!    they share with other programs (an editor's scope).
//!
//! A cgroup path is the strongest signal: it is inherited at fork and moves
//! only when something writes cgroup.procs. The parent pid is weaker (an
//! orphan is reparented to pid 1 or a subreaper, proc_pid_stat(5) and
//! PR_SET_CHILD_SUBREAPER(2const)), so ancestry only refines three classes
//! (see `rule_agent_ancestry`) and its confidence is "medium".
//!
//! A cgroup that only groups others (`/system.slice`, `app.slice`, the user
//! manager's own directory) names no unit and no workload: it is
//! Unclassified here on purpose, so that a class's totals can come from the
//! topmost cgroups that do name one.
//!
//! Keys are slices of the input path or comm, still escaped as the cgroup
//! directory holds them; decode with `unit_path.unescape`. Nothing here
//! allocates except the ancestry index of `classifyProcesses`.
const std = @import("std");
const Allocator = std.mem.Allocator;
const unit_path = @import("unit_path.zig");
const walk = @import("walk.zig");

pub const Class = enum(u8) {
    kernel,
    init,
    system_services,
    session_services,
    applications,
    login_session,
    agent_sessions,
    containers_and_vms,
    unclassified,
};

/// Display names, indexed by `Class`.
pub const class_labels = [_][]const u8{
    "Kernel",
    "Init",
    "System services",
    "Session services",
    "Applications",
    "Login session",
    "Agent sessions",
    "Containers and VMs",
    "Unclassified",
};

comptime {
    std.debug.assert(class_labels.len == @typeInfo(Class).@"enum".fields.len);
}

pub fn classLabel(class: Class) []const u8 {
    return class_labels[@intFromEnum(class)];
}

/// How sure the rule is that the class is right. `none` is only the last
/// rule's: nothing matched.
pub const Confidence = enum(u8) { none, low, medium, high };

/// Names, indexed by `Confidence`.
pub const confidence_names = [_][]const u8{ "none", "low", "medium", "high" };

comptime {
    std.debug.assert(confidence_names.len == @typeInfo(Confidence).@"enum".fields.len);
}

/// What `Classification.key` holds.
pub const KeyKind = enum(u8) {
    none,
    /// A systemd unit name (`ssh.service`).
    unit,
    /// The application id of an `app-*` unit.
    app_id,
    /// A login session id (`c2`).
    session,
    /// A scope or cgroup directory name (an agent session, a machine, a container).
    scope,
    /// The project of an agent slice (`agents-P.slice`).
    project,
    /// An agent program: `key` is its comm, `agent_pid` and
    /// `agent_start_ticks` identify it.
    agent_process,
};

pub const Classification = struct {
    class: Class,
    /// Index into `rules`.
    rule: u8,
    confidence: Confidence,
    key_kind: KeyKind = .none,
    /// A slice of the cgroup path (or of the agent's comm): see `KeyKind`.
    key: []const u8 = "",
    /// With `key_kind == .agent_process`: the outermost agent program of the
    /// process's ancestry; (pid, start_ticks) survives pid reuse.
    agent_pid: u32 = 0,
    agent_start_ticks: u64 = 0,
    /// A node of the cgroup tree that holds several classes (the root,
    /// `user.slice`, `user-N.slice`, a user manager, `app.slice`): neither a
    /// class nor Unclassified; totals recurse into its children. `class` is
    /// then only a placeholder. A process is never mixed.
    mixed: bool = false,
};

pub const Pass = enum { path, process };

/// What a path rule found: the key, and a confidence when it differs from
/// the rule's own.
pub const Match = struct {
    key: []const u8 = "",
    confidence: ?Confidence = null,
};

pub const Rule = struct {
    name: []const u8,
    class: Class,
    confidence: Confidence,
    key_kind: KeyKind,
    pass: Pass,
    /// The path test of a path rule; null for process rules (decided in
    /// `classifyProcess`) and for the last rule (everything else).
    match: ?*const fn (path: []const u8) ?Match = null,
};

/// Indexes into `rules`; the rule numbers of the design are these plus one.
pub const rule_kernel_thread = 0;
pub const rule_init = 1;
pub const rule_agent_session_scope = 2;
pub const rule_agent_job_scope = 3;
pub const rule_agent_slice = 4;
pub const rule_machine = 5;
pub const rule_system_service = 6;
pub const rule_user_manager = 7;
pub const rule_session_service = 8;
pub const rule_background_task = 9;
pub const rule_application = 10;
pub const rule_user_service = 11;
pub const rule_other_user_unit = 12;
pub const rule_login_session = 13;
pub const rule_agent_ancestry = 14;
pub const rule_no_systemd_service = 15;
pub const rule_no_cgroup_system = 16;
pub const rule_no_cgroup_login = 17;
pub const rule_mixed = 18;
pub const rule_otherwise = 19;

/// First match wins. Path rules are tried in this order by the path pass; the
/// process rules are applied by `classifyProcess` at the places the order
/// puts them (see there).
pub const rules = [_]Rule{
    .{ .name = "kernel thread", .class = .kernel, .confidence = .high, .key_kind = .none, .pass = .process },
    .{ .name = "init", .class = .init, .confidence = .high, .key_kind = .none, .pass = .path, .match = matchInit },
    .{ .name = "agent session scope", .class = .agent_sessions, .confidence = .high, .key_kind = .scope, .pass = .path, .match = matchAgentSessionScope },
    .{ .name = "agent job scope", .class = .agent_sessions, .confidence = .high, .key_kind = .scope, .pass = .path, .match = matchAgentJobScope },
    .{ .name = "agent slice", .class = .agent_sessions, .confidence = .high, .key_kind = .project, .pass = .path, .match = matchAgentSlice },
    .{ .name = "machine", .class = .containers_and_vms, .confidence = .high, .key_kind = .scope, .pass = .path, .match = matchMachine },
    .{ .name = "system service", .class = .system_services, .confidence = .high, .key_kind = .unit, .pass = .path, .match = matchSystemService },
    .{ .name = "user manager", .class = .session_services, .confidence = .high, .key_kind = .none, .pass = .path, .match = matchUserManager },
    .{ .name = "session service", .class = .session_services, .confidence = .high, .key_kind = .unit, .pass = .path, .match = matchSessionService },
    .{ .name = "background task", .class = .session_services, .confidence = .high, .key_kind = .unit, .pass = .path, .match = matchBackgroundTask },
    .{ .name = "application", .class = .applications, .confidence = .high, .key_kind = .app_id, .pass = .path, .match = matchApplication },
    .{ .name = "user service", .class = .session_services, .confidence = .medium, .key_kind = .unit, .pass = .path, .match = matchUserService },
    .{ .name = "other user unit", .class = .session_services, .confidence = .low, .key_kind = .unit, .pass = .path, .match = matchOtherUserUnit },
    .{ .name = "login session", .class = .login_session, .confidence = .medium, .key_kind = .session, .pass = .path, .match = matchLoginSession },
    .{ .name = "agent ancestry", .class = .agent_sessions, .confidence = .medium, .key_kind = .agent_process, .pass = .process },
    .{ .name = "no systemd: service cgroup", .class = .system_services, .confidence = .low, .key_kind = .unit, .pass = .path, .match = matchNoSystemdService },
    .{ .name = "no cgroup: system", .class = .system_services, .confidence = .low, .key_kind = .none, .pass = .process },
    .{ .name = "no cgroup: login", .class = .login_session, .confidence = .low, .key_kind = .none, .pass = .process },
    .{ .name = "mixed node", .class = .unclassified, .confidence = .none, .key_kind = .none, .pass = .path, .match = matchMixed },
    .{ .name = "otherwise", .class = .unclassified, .confidence = .none, .key_kind = .none, .pass = .path },
};

comptime {
    std.debug.assert(rules.len == rule_otherwise + 1);
    std.debug.assert(rules[rule_kernel_thread].class == .kernel);
    std.debug.assert(rules[rule_agent_ancestry].pass == .process);
    std.debug.assert(rules[rule_no_cgroup_login].class == .login_session);
    std.debug.assert(rules[rule_mixed].match != null);
}

// ---- helpers over path components ----

fn components(path: []const u8) std.mem.TokenIterator(u8, .scalar) {
    return std.mem.tokenizeScalar(u8, path, '/');
}

fn first(path: []const u8) ?[]const u8 {
    var it = components(path);
    return it.next();
}

fn isFirst(path: []const u8, name: []const u8) bool {
    const component = first(path) orelse return false;
    return std.mem.eql(u8, component, name);
}

/// The text of a path with a `.scope` or `.service` suffix removed, or null.
fn stripSuffix(name: []const u8, suffix: []const u8) ?[]const u8 {
    if (!std.mem.endsWith(u8, name, suffix)) return null;
    return name[0 .. name.len - suffix.len];
}

/// A path made of slices only: a grouping node that names no unit. The root
/// has no components and is not one.
fn onlySlices(path: []const u8) bool {
    var it = components(path);
    if (it.peek() == null) return false;
    return unit_path.skipSlices(path).len == 0;
}

// ---- agentres-style agent scopes: TOOL-PROJECT-PID.scope and job-PROJECT-PID.scope ----

pub const AgentScope = struct {
    /// `claude`, `opencode`, `pi`, or `job` for a heavy job's scope.
    tool: []const u8,
    /// Still escaped (a project name's hyphens are `\x2d`).
    project: []const u8,
    pid: u32,
};

/// Splits `TOOL-PROJECT-PID.scope` at its first and last hyphen. Null when
/// the name has another shape.
pub fn parseAgentScope(component: []const u8) ?AgentScope {
    const stem = stripSuffix(component, ".scope") orelse return null;
    const first_hyphen = std.mem.indexOfScalar(u8, stem, '-') orelse return null;
    const last_hyphen = std.mem.lastIndexOfScalar(u8, stem, '-').?;
    if (first_hyphen == last_hyphen or first_hyphen == 0) return null;
    const project = stem[first_hyphen + 1 .. last_hyphen];
    const pid_text = stem[last_hyphen + 1 ..];
    if (project.len == 0) return null;
    for (pid_text) |byte| if (!std.ascii.isDigit(byte)) return null;
    const pid = std.fmt.parseInt(u32, pid_text, 10) catch return null;
    return .{ .tool = stem[0..first_hyphen], .project = project, .pid = pid };
}

/// The agent scope under a user manager at `agents.slice/agents-P.slice/`.
fn agentScopeOf(path: []const u8) ?struct { name: []const u8, scope: AgentScope } {
    const rest = unit_path.userManagerRest(path) orelse return null;
    var it = components(rest);
    const root = it.next() orelse return null;
    if (!std.mem.eql(u8, root, "agents.slice")) return null;
    const project_slice = it.next() orelse return null;
    if (!std.mem.startsWith(u8, project_slice, "agents-") or !std.mem.endsWith(u8, project_slice, ".slice")) return null;
    if (project_slice.len <= "agents-.slice".len) return null;
    const name = it.next() orelse return null;
    const scope = parseAgentScope(name) orelse return null;
    return .{ .name = name, .scope = scope };
}

fn matchAgentSessionScope(path: []const u8) ?Match {
    const found = agentScopeOf(path) orelse return null;
    if (std.mem.eql(u8, found.scope.tool, "job")) return null;
    return .{ .key = found.name };
}

fn matchAgentJobScope(path: []const u8) ?Match {
    const found = agentScopeOf(path) orelse return null;
    if (!std.mem.eql(u8, found.scope.tool, "job")) return null;
    return .{ .key = found.name };
}

/// `agents.slice` and `agents-P.slice`: every cgroup below holds agent
/// sessions, so the slices carry the class; the project slice's key is P.
fn matchAgentSlice(path: []const u8) ?Match {
    const rest = unit_path.userManagerRest(path) orelse return null;
    if (!isFirst(rest, "agents.slice") or !onlySlices(rest)) return null;
    var it = components(rest);
    _ = it.next();
    const project_slice = it.next() orelse return .{};
    const prefix = "agents-";
    const suffix = ".slice";
    if (!std.mem.startsWith(u8, project_slice, prefix) or project_slice.len <= prefix.len + suffix.len) return .{};
    return .{ .key = project_slice[prefix.len .. project_slice.len - suffix.len] };
}

// ---- the other path rules ----

fn matchInit(path: []const u8) ?Match {
    if (!isFirst(path, "init.scope")) return null;
    return .{};
}

fn isRuntimeScope(component: []const u8, prefix: []const u8) bool {
    return component.len > prefix.len + ".scope".len and
        std.mem.startsWith(u8, component, prefix) and std.mem.endsWith(u8, component, ".scope");
}

fn matchMachine(path: []const u8) ?Match {
    var it = components(path);
    const top = it.next() orelse return null;
    // systemd-machined's own slice: high confidence; the key is the machine's scope.
    if (std.mem.eql(u8, top, "machine.slice")) {
        const machine = it.next() orelse return .{ .confidence = .high };
        return .{ .key = machine, .confidence = .high };
    }
    // The cgroupfs drivers of docker and LXC put containers under a directory of that name.
    if (std.mem.eql(u8, top, "docker") or std.mem.eql(u8, top, "lxc")) {
        const container = it.next() orelse return .{ .confidence = .medium };
        return .{ .key = container, .confidence = .medium };
    }
    it.reset();
    while (it.next()) |component| {
        const named = isRuntimeScope(component, "docker-") or isRuntimeScope(component, "libpod-") or
            std.mem.startsWith(u8, component, "kubepods");
        if (named) return .{ .key = component, .confidence = .medium };
    }
    return null;
}

fn matchSystemService(path: []const u8) ?Match {
    if (!isFirst(path, "system.slice")) return null;
    // `system.slice` and the slices under it hold nothing but services.
    if (onlySlices(path)) return .{};
    const name = unit_path.unit(path) orelse return null;
    return .{ .key = name };
}

fn matchUserManager(path: []const u8) ?Match {
    const rest = unit_path.userManagerRest(path) orelse return null;
    if (!isFirst(rest, "init.scope")) return null;
    return .{};
}

/// What lies in `<manager>/<slice_name>/`: the unit below it, or the bare
/// slice (and slices below it) when it names no unit, which carries the
/// class of everything inside.
fn matchUserSlice(path: []const u8, slice_name: []const u8) ?Match {
    const rest = unit_path.userManagerRest(path) orelse return null;
    if (!isFirst(rest, slice_name)) return null;
    if (onlySlices(rest)) return .{};
    return .{ .key = unit_path.unit(rest) orelse return null };
}

fn matchSessionService(path: []const u8) ?Match {
    return matchUserSlice(path, "session.slice");
}

fn matchBackgroundTask(path: []const u8) ?Match {
    return matchUserSlice(path, "background.slice");
}

/// The AppID of `app[-LAUNCHER]-APPID-RANDOM.scope` or
/// `app[-LAUNCHER]-APPID[@RANDOM].service`, or of an application's slice
/// `app[-LAUNCHER]-APPID.slice`. A hyphen inside a part is
/// escaped (`\x2d`) in a unit name, so hyphens split the parts reliably.
pub fn parseAppId(unit_name: []const u8) ?[]const u8 {
    if (!std.mem.startsWith(u8, unit_name, "app-")) return null;
    var parts: [4][]const u8 = undefined;
    var count: usize = 0;
    var core: []const u8 = undefined;
    var min_parts: usize = undefined;
    var max_parts: usize = undefined;
    if (stripSuffix(unit_name, ".scope")) |stem| {
        core = stem["app-".len..];
        // [LAUNCHER-]APPID-RANDOM
        min_parts = 2;
        max_parts = 3;
    } else if (stripSuffix(unit_name, ".slice")) |stem| {
        // app[-LAUNCHER]-APPID.slice
        core = stem["app-".len..];
        min_parts = 1;
        max_parts = 2;
    } else if (stripSuffix(unit_name, ".service")) |stem| {
        core = stem["app-".len..];
        if (std.mem.indexOfScalar(u8, core, '@')) |at| core = core[0..at];
        // [LAUNCHER-]APPID
        min_parts = 1;
        max_parts = 2;
    } else return null;

    var it = std.mem.splitScalar(u8, core, '-');
    while (it.next()) |part| {
        if (part.len == 0 or count == parts.len) return null;
        parts[count] = part;
        count += 1;
    }
    if (count < min_parts or count > max_parts) return null;
    // Scopes end in RANDOM, services in APPID.
    return if (min_parts == 2) parts[count - 2] else parts[count - 1];
}

fn matchApplication(path: []const u8) ?Match {
    const rest = unit_path.userManagerRest(path) orelse return null;
    if (!isFirst(rest, "app.slice")) return null;
    var it = components(rest);
    _ = it.next();
    // An application's own slice, and every unit inside it (a terminal puts
    // each tab's shell in `vte-spawn-*.scope` under `app-org.gnome.Terminal.slice`).
    // That layout is the launcher's convention, not systemd's naming: medium.
    if (it.next()) |second| {
        if (unit_path.isSliceName(second)) {
            if (parseAppId(second)) |app_id| return .{ .key = app_id, .confidence = .medium };
        }
    }
    const name = unit_path.unit(rest) orelse return null;
    return .{ .key = parseAppId(name) orelse return null };
}

fn matchUserService(path: []const u8) ?Match {
    const rest = unit_path.userManagerRest(path) orelse return null;
    if (!isFirst(rest, "app.slice")) return null;
    return .{ .key = unit_path.unit(rest) orelse return null };
}

fn matchOtherUserUnit(path: []const u8) ?Match {
    const rest = unit_path.userManagerRest(path) orelse return null;
    // `app.slice` itself holds applications and services alike: mixed.
    if (isMixedAppSlice(rest)) return null;
    // A bare slice below the manager (its `user.slice`) holds only such units.
    if (onlySlices(rest)) return .{};
    return .{ .key = unit_path.unit(rest) orelse return null };
}

fn isMixedAppSlice(rest_of_manager: []const u8) bool {
    var it = components(rest_of_manager);
    const only = it.next() orelse return false;
    return std.mem.eql(u8, only, "app.slice") and it.next() == null;
}

fn matchLoginSession(path: []const u8) ?Match {
    _ = unit_path.ownerUid(path) orelse return null;
    return .{ .key = unit_path.session(path) orelse return null };
}

/// The nodes that hold several classes: the root, `user.slice`, `user-N.slice`,
/// a user manager's directory and its `app.slice`.
fn matchMixed(path: []const u8) ?Match {
    var it = components(path);
    const top = it.next() orelse return .{}; // the root
    if (unit_path.userManagerRest(path)) |rest| {
        return if (rest.len == 0 or isMixedAppSlice(rest)) .{} else null;
    }
    if (!std.mem.eql(u8, top, "user.slice")) return null;
    const user_slice = it.next() orelse return .{};
    if (it.next() != null) return null;
    return if (unit_path.ownerUid(path) != null and unit_path.isSliceName(user_slice)) .{} else null;
}

/// A top-level cgroup that is no systemd slice, as OpenRC and similar init
/// systems make one per service. Container managers' parent directories are
/// not services.
fn matchNoSystemdService(path: []const u8) ?Match {
    const top = first(path) orelse return null;
    if (unit_path.isSliceName(top)) return null;
    if (std.mem.eql(u8, top, "docker") or std.mem.eql(u8, top, "lxc")) return null;
    return .{ .key = top };
}

// ---- the passes ----

fn fromRule(index: usize, found: Match) Classification {
    const rule = rules[index];
    return .{
        .class = rule.class,
        .rule = @intCast(index),
        .confidence = found.confidence orelse rule.confidence,
        .key_kind = if (found.key.len == 0) .none else rule.key_kind,
        .key = found.key,
        .mixed = index == rule_mixed,
    };
}

fn processOnly(index: usize) Classification {
    return fromRule(index, .{});
}

/// The first path rule that matches, or null (the "otherwise" rule is not
/// tried: callers decide what no match means).
fn matchPathRules(path: []const u8) ?Classification {
    for (rules, 0..) |rule, index| {
        if (rule.pass != .path) continue;
        const matcher = rule.match orelse continue;
        const found = matcher(path) orelse continue;
        return fromRule(index, found);
    }
    return null;
}

/// The class of a cgroup path by the path rules alone. A path no rule names
/// (the root, a slice that only groups others) is Unclassified.
pub fn classifyPath(path: []const u8) Classification {
    return matchPathRules(path) orelse processOnly(rule_otherwise);
}

/// The agent programs, by `comm` (the name the kernel shows, which is the
/// program's own name, not the version-numbered file it runs from).
pub const agent_comms = [_][]const u8{ "claude", "opencode", "pi" };

fn isAgentComm(comm: []const u8) bool {
    for (agent_comms) |name| if (std.mem.eql(u8, comm, name)) return true;
    return false;
}

/// Uids below this are system accounts (the usual SYS_UID_MAX + 1).
const first_login_uid = 1000;

fn hasControllingTty(process: *const walk.Process) bool {
    return process.tty_nr != 0;
}

/// A cgroup path that tells where a process runs: a missing one, and the
/// root `/` (a non-systemd machine's catch-all), tell nothing.
fn usablePath(process: *const walk.Process) ?[]const u8 {
    const path = process.cgroup_path orelse return null;
    var it = components(path);
    if (it.next() == null) return null;
    return path;
}

fn canBecomeAgent(class: Class) bool {
    return class == .applications or class == .session_services or class == .login_session;
}

/// One process. `outer_agent` is the outermost agent program among the
/// process's ancestors and itself, when there is one.
fn classifyProcess(process: *const walk.Process, outer_agent: ?*const walk.Process) Classification {
    // 1 and 2: what the kernel says about the process itself.
    if (process.isKernelThread() or process.pid == 2) return processOnly(rule_kernel_thread);
    if (process.pid == 1) return processOnly(rule_init);

    // 3 to 13 and 15: the path.
    if (usablePath(process)) |path| {
        const by_path = matchPathRules(path) orelse return processOnly(rule_otherwise);
        // A process is never "mixed": in a node that holds several classes
        // it is unclassified, as there is nothing more to go on.
        if (by_path.mixed) return processOnly(rule_otherwise);
        // 14: ancestry refines the three classes that can hold an agent's
        // processes beside other programs; agent scopes are already exact.
        if (outer_agent) |agent| {
            if (canBecomeAgent(by_path.class)) {
                var refined = fromRule(rule_agent_ancestry, .{ .key = agent.comm() });
                refined.agent_pid = agent.pid;
                refined.agent_start_ticks = agent.start_ticks;
                return refined;
            }
        }
        return by_path;
    }

    // 16 and 17: no cgroup information, so the terminal and the uid. An
    // unknown uid is not guessed.
    if (hasControllingTty(process)) return processOnly(rule_no_cgroup_login);
    const status = process.status orelse return processOnly(rule_otherwise);
    const uid = status.uid_real orelse return processOnly(rule_otherwise);
    if (uid < first_login_uid) return processOnly(rule_no_cgroup_system);
    return processOnly(rule_otherwise);
}

/// Finds, for a process, the outermost ancestor-or-self whose comm is an
/// agent program, within one snapshot. Linear in the number of processes:
/// each process is resolved once and remembered.
const Ancestry = struct {
    processes: []const walk.Process,
    by_pid: std.AutoHashMapUnmanaged(u32, u32),
    /// Per process: the answer, or `unvisited` / `in_progress` / `no_agent`.
    memo: []u32,
    stack: []u32,

    const unvisited = std.math.maxInt(u32);
    const in_progress = unvisited - 1;
    const no_agent = unvisited - 2;

    fn init(gpa: Allocator, processes: []const walk.Process) Allocator.Error!Ancestry {
        const count = processes.len;
        // Indexes are u32; a snapshot cannot hold more processes than pids.
        std.debug.assert(count < no_agent);
        var by_pid: std.AutoHashMapUnmanaged(u32, u32) = .empty;
        errdefer by_pid.deinit(gpa);
        try by_pid.ensureTotalCapacity(gpa, @intCast(count));
        const both = try gpa.alloc(u32, 2 * count);
        errdefer gpa.free(both);
        for (processes, 0..) |process, index| by_pid.putAssumeCapacity(process.pid, @intCast(index));
        @memset(both[0..count], unvisited);
        return .{ .processes = processes, .by_pid = by_pid, .memo = both[0..count], .stack = both[count..] };
    }

    fn deinit(self: *Ancestry, gpa: Allocator) void {
        gpa.free(self.memo.ptr[0 .. 2 * self.memo.len]);
        self.by_pid.deinit(gpa);
    }

    /// A parent is a process with the pid `ppid` that started no later than
    /// the child: a later one is a reused pid, not the parent. pid 0, the
    /// process itself and a pid not in the snapshot have no parent here.
    fn parentOf(self: *const Ancestry, index: u32) ?u32 {
        const child = &self.processes[index];
        if (child.ppid == 0 or child.ppid == child.pid) return null;
        const parent_index = self.by_pid.get(child.ppid) orelse return null;
        if (self.processes[parent_index].start_ticks > child.start_ticks) return null;
        return parent_index;
    }

    fn outermostAgent(self: *Ancestry, start: u32) ?u32 {
        // Climb until a remembered answer, the top, or a cycle.
        var depth: usize = 0;
        var base: u32 = no_agent;
        var current = start;
        while (true) {
            const known = self.memo[current];
            if (known == in_progress) break;
            if (known != unvisited) {
                base = known;
                break;
            }
            self.memo[current] = in_progress;
            self.stack[depth] = current;
            depth += 1;
            current = self.parentOf(current) orelse break;
        }
        // Come back down: the outermost agent above wins, else the process if it is one.
        while (depth > 0) {
            depth -= 1;
            const index = self.stack[depth];
            if (base == no_agent and isAgentComm(self.processes[index].comm())) base = index;
            self.memo[index] = base;
        }
        return if (base == no_agent) null else base;
    }
};

pub const Error = Allocator.Error || error{LengthMismatch};

/// Classifies every process into `out` (one entry per process, same order).
/// Keys point into `processes` (their cgroup paths and comms), which must
/// outlive `out`. Allocates only the ancestry index, and only when some
/// process is an agent program.
pub fn classifyProcesses(gpa: Allocator, processes: []const walk.Process, out: []Classification) Error!void {
    if (out.len != processes.len) return error.LengthMismatch;

    var any_agent = false;
    for (processes) |process| {
        if (isAgentComm(process.comm())) {
            any_agent = true;
            break;
        }
    }
    if (!any_agent) {
        for (processes, out) |*process, *slot| slot.* = classifyProcess(process, null);
        return;
    }

    var ancestry = try Ancestry.init(gpa, processes);
    defer ancestry.deinit(gpa);
    for (processes, out, 0..) |*process, *slot, index| {
        const outer = ancestry.outermostAgent(@intCast(index));
        slot.* = classifyProcess(process, if (outer) |agent_index| &processes[agent_index] else null);
    }
}

// ---- tests ----

const testing = std.testing;

const user_manager = "/user.slice/user-1000.slice/user@1000.service";

fn expectPath(path: []const u8, rule_index: usize, class: Class, confidence: Confidence, key: []const u8) !void {
    const got = classifyPath(path);
    errdefer std.debug.print("path: {s}\n", .{path});
    try testing.expectEqualStrings(rules[rule_index].name, rules[got.rule].name);
    try testing.expectEqual(class, got.class);
    try testing.expectEqual(confidence, got.confidence);
    try testing.expectEqualStrings(key, got.key);
}

test "the rule table is in the designed order with its names" {
    const names = [_][]const u8{
        "kernel thread",     "init",             "agent session scope", "agent job scope",
        "agent slice",       "machine",          "system service",      "user manager",
        "session service",   "background task",  "application",         "user service",
        "other user unit",   "login session",    "agent ancestry",      "no systemd: service cgroup",
        "no cgroup: system", "no cgroup: login", "mixed node",          "otherwise",
    };
    try testing.expectEqual(names.len, rules.len);
    for (names, rules) |name, rule| try testing.expectEqualStrings(name, rule.name);
}

test "system services are keyed by their unit, through nested slices and sub-cgroups" {
    try expectPath("/system.slice/ssh.service", rule_system_service, .system_services, .high, "ssh.service");
    try expectPath("/system.slice/system-postgresql.slice/postgresql@16-main.service", rule_system_service, .system_services, .high, "postgresql@16-main.service");
    try expectPath("/system.slice/systemd-udevd.service/udev", rule_system_service, .system_services, .high, "systemd-udevd.service");
}

test "init, the user manager and the session's own services" {
    try expectPath("/init.scope", rule_init, .init, .high, "");
    try expectPath(user_manager ++ "/init.scope", rule_user_manager, .session_services, .high, "");
    try expectPath(user_manager ++ "/session.slice/pipewire.service", rule_session_service, .session_services, .high, "pipewire.service");
    try expectPath(user_manager ++ "/background.slice/tracker-miner-fs-3.service", rule_background_task, .session_services, .high, "tracker-miner-fs-3.service");
}

test "applications are keyed by their AppID, launcher and random suffix removed" {
    try expectPath(user_manager ++ "/app.slice/app-com.visualstudio.CodeOSS-4247.scope", rule_application, .applications, .high, "com.visualstudio.CodeOSS");
    try expectPath(user_manager ++ "/app.slice/app-gnome-org.gnome.Terminal-8123.scope", rule_application, .applications, .high, "org.gnome.Terminal");
    try expectPath(user_manager ++ "/app.slice/app-flatpak-org.mozilla.firefox-9001.scope/leaf", rule_application, .applications, .high, "org.mozilla.firefox");
    try expectPath(user_manager ++ "/app.slice/app-gnome-gnome\\x2dtweaks@7.service", rule_application, .applications, .high, "gnome\\x2dtweaks");
    try expectPath(user_manager ++ "/app.slice/app-firefox.service", rule_application, .applications, .high, "firefox");
}

test "a daemon in app.slice is a user service, not an application" {
    try expectPath(user_manager ++ "/app.slice/dconf.service", rule_user_service, .session_services, .medium, "dconf.service");
    try expectPath(user_manager ++ "/app.slice/dbus.socket", rule_user_service, .session_services, .medium, "dbus.socket");
    // An app-* name of the wrong shape is no application.
    try expectPath(user_manager ++ "/app.slice/app-a-b-c-d.scope", rule_user_service, .session_services, .medium, "app-a-b-c-d.scope");
}

test "anything else under the user manager is a low-confidence session service" {
    try expectPath(user_manager ++ "/user.slice/podman-pause-7642ff7d.scope", rule_other_user_unit, .session_services, .low, "podman-pause-7642ff7d.scope");
}

fn expectMixed(path: []const u8) !void {
    const got = classifyPath(path);
    errdefer std.debug.print("path: {s}\n", .{path});
    try testing.expect(got.mixed);
    try testing.expectEqual(@as(u8, rule_mixed), got.rule);
    try testing.expectEqual(Confidence.none, got.confidence);
}

test "nodes that hold several classes are mixed, which is not a class" {
    const mixed = [_][]const u8{
        "/",
        "",
        "/user.slice",
        "/user.slice/user-1000.slice",
        user_manager,
        user_manager ++ "/app.slice",
    };
    for (mixed) |path| try expectMixed(path);
    // Everything else is not mixed.
    try testing.expect(!classifyPath("/system.slice").mixed);
    try testing.expect(!classifyPath(user_manager ++ "/app.slice/dconf.service").mixed);
    try testing.expect(!classifyPath("/user.slice/user-1000.slice/session-c2.scope").mixed);
    try testing.expect(!classifyPath("/user.slice/user-abc.slice").mixed);
}

test "a slice whose whole subtree is one class carries that class" {
    try expectPath("/system.slice", rule_system_service, .system_services, .high, "");
    try expectPath("/system.slice/system-postgresql.slice", rule_system_service, .system_services, .high, "");
    try expectPath("/machine.slice", rule_machine, .containers_and_vms, .high, "");
    try expectPath("/docker", rule_machine, .containers_and_vms, .medium, "");
    try expectPath("/lxc", rule_machine, .containers_and_vms, .medium, "");
    try expectPath(user_manager ++ "/session.slice", rule_session_service, .session_services, .high, "");
    try expectPath(user_manager ++ "/background.slice", rule_background_task, .session_services, .high, "");
    try expectPath(user_manager ++ "/agents.slice", rule_agent_slice, .agent_sessions, .high, "");
    try expectPath(user_manager ++ "/agents.slice/agents-alpha.slice", rule_agent_slice, .agent_sessions, .high, "alpha");
    try expectPath(user_manager ++ "/app.slice/app-org.gnome.Terminal.slice", rule_application, .applications, .medium, "org.gnome.Terminal");
    // The manager's other slice holds only odd units.
    try expectPath(user_manager ++ "/user.slice", rule_other_user_unit, .session_services, .low, "");
}

test "units inside an application's slice are that application, at medium confidence" {
    const terminal = user_manager ++ "/app.slice/app-gnome-org.gnome.Terminal.slice";
    try expectPath(terminal ++ "/vte-spawn-0a1b2c3d.scope", rule_application, .applications, .medium, "org.gnome.Terminal");
    try expectPath(terminal ++ "/vte-spawn-0a1b2c3d.scope/leaf", rule_application, .applications, .medium, "org.gnome.Terminal");
    // A slice that is not an application's does not make its units one.
    try expectPath(user_manager ++ "/app.slice/other.slice/x.service", rule_user_service, .session_services, .medium, "x.service");
    // An application's slice name of the wrong shape does not either.
    try expectPath(user_manager ++ "/app.slice/app-a-b-c.slice/x.service", rule_user_service, .session_services, .medium, "x.service");
}

test "a slice no rule knows is unclassified" {
    try expectPath("/unknown.slice", rule_otherwise, .unclassified, .none, "");
    try testing.expect(!classifyPath("/unknown.slice").mixed);
}

test "agent scopes and jobs are exact and keyed by the scope" {
    const slice = user_manager ++ "/agents.slice/agents-alpha.slice";
    try expectPath(slice ++ "/claude-alpha-4242.scope", rule_agent_session_scope, .agent_sessions, .high, "claude-alpha-4242.scope");
    try expectPath(slice ++ "/claude-alpha-4242.scope/leaf", rule_agent_session_scope, .agent_sessions, .high, "claude-alpha-4242.scope");
    try expectPath(slice ++ "/opencode-alpha-7.scope", rule_agent_session_scope, .agent_sessions, .high, "opencode-alpha-7.scope");
    try expectPath(slice ++ "/job-alpha-4300.scope", rule_agent_job_scope, .agent_sessions, .high, "job-alpha-4300.scope");
    // Not of the shape: an ordinary user unit.
    try expectPath(slice ++ "/stray.service", rule_other_user_unit, .session_services, .low, "stray.service");
    try expectPath(slice ++ "/claude-alpha-x.scope", rule_other_user_unit, .session_services, .low, "claude-alpha-x.scope");
}

test "an agent scope name splits into tool, project and pid" {
    const scope = parseAgentScope("claude-gnome\\x2dsystem-4242.scope").?;
    try testing.expectEqualStrings("claude", scope.tool);
    try testing.expectEqualStrings("gnome\\x2dsystem", scope.project);
    try testing.expectEqual(@as(u32, 4242), scope.pid);
    try testing.expectEqual(@as(?AgentScope, null), parseAgentScope("claude-4242.scope"));
    try testing.expectEqual(@as(?AgentScope, null), parseAgentScope("claude--4242.scope"));
    try testing.expectEqual(@as(?AgentScope, null), parseAgentScope("-alpha-4242.scope"));
    try testing.expectEqual(@as(?AgentScope, null), parseAgentScope("claude-alpha-99999999999.scope"));
    try testing.expectEqual(@as(?AgentScope, null), parseAgentScope("claude-alpha-1.service"));
}

test "machines and containers: systemd's with high confidence, the others' with medium" {
    try expectPath("/machine.slice/machine-qemu\\x2d1\\x2dvm1.scope", rule_machine, .containers_and_vms, .high, "machine-qemu\\x2d1\\x2dvm1.scope");
    try expectPath("/machine.slice/machine-qemu\\x2d1\\x2dvm1.scope/libvirt/emulator", rule_machine, .containers_and_vms, .high, "machine-qemu\\x2d1\\x2dvm1.scope");
    try expectPath("/system.slice/docker-0123abcd.scope", rule_machine, .containers_and_vms, .medium, "docker-0123abcd.scope");
    try expectPath("/system.slice/libpod-4567cdef.scope", rule_machine, .containers_and_vms, .medium, "libpod-4567cdef.scope");
    try expectPath("/kubepods.slice/kubepods-burstable.slice/kubepods-burstable-pod1.slice/cri-containerd-89ab.scope", rule_machine, .containers_and_vms, .medium, "kubepods.slice");
    try expectPath("/docker/0123abcd", rule_machine, .containers_and_vms, .medium, "0123abcd");
    try expectPath("/lxc/web1", rule_machine, .containers_and_vms, .medium, "web1");
    // The docker daemon is a system service, not a container.
    try expectPath("/system.slice/docker.service", rule_system_service, .system_services, .high, "docker.service");
    try expectPath("/system.slice/docker.socket", rule_system_service, .system_services, .high, "docker.socket");
}

test "login sessions are keyed by the session id" {
    try expectPath("/user.slice/user-1000.slice/session-c2.scope", rule_login_session, .login_session, .medium, "c2");
    try expectPath("/user.slice/user-0.slice/session-1.scope", rule_login_session, .login_session, .medium, "1");
    try expectPath("/user.slice/user-1000.slice/session-c2.scope/leaf", rule_login_session, .login_session, .medium, "c2");
}

test "without systemd a top-level cgroup is a low-confidence service" {
    try expectPath("/openrc.sshd", rule_no_systemd_service, .system_services, .low, "openrc.sshd");
    try expectPath("/openrc.sshd/child", rule_no_systemd_service, .system_services, .low, "openrc.sshd");
}

// ---- the captured path list ----

fn readPathList(gpa: Allocator) ![]u8 {
    return std.Io.Dir.cwd().readFileAlloc(testing.io, "testdata/cgroup-paths.txt", gpa, .limited(1 << 20));
}

test "every captured cgroup path is classified; only four grouping nodes are mixed" {
    const text = try readPathList(testing.allocator);
    defer testing.allocator.free(text);

    var counted = [_]u32{0} ** class_labels.len;
    var mixed_count: u32 = 0;
    var lines = std.mem.splitScalar(u8, text, '\n');
    while (lines.next()) |line| {
        if (line.len == 0 or line[0] == '#') continue;
        const got = classifyPath(line);
        errdefer std.debug.print("path: {s}\n", .{line});
        // A key is always a piece of the path.
        try testing.expect(std.mem.indexOf(u8, line, got.key) != null);
        try testing.expect(got.class != .unclassified or got.mixed);
        if (!got.mixed) {
            counted[@intFromEnum(got.class)] += 1;
            continue;
        }
        // Mixed: user.slice, user-N.slice, a user manager, its app.slice.
        mixed_count += 1;
        const last = line[std.mem.lastIndexOfScalar(u8, line, '/').? + 1 ..];
        const known = std.mem.eql(u8, line, "/user.slice") or std.mem.startsWith(u8, line, "/user.slice/user-") and
            std.mem.endsWith(u8, line, ".slice") and std.mem.count(u8, line, "/") == 2 or
            std.mem.startsWith(u8, last, "user@") or std.mem.endsWith(u8, line, "/app.slice");
        try testing.expect(known);
    }
    try testing.expectEqual(@as(u32, 4), mixed_count);
    // The capture holds every class a path can give.
    inline for (.{ .init, .system_services, .session_services, .applications, .login_session, .agent_sessions, .containers_and_vms }) |class| {
        try testing.expect(counted[@intFromEnum(@as(Class, class))] > 0);
    }
    try testing.expectEqual(@as(u32, 0), counted[@intFromEnum(Class.unclassified)]);
}

test "samples of the captured list land on the rules that name them" {
    try expectPath("/system.slice/game-server.service", rule_system_service, .system_services, .high, "game-server.service");
    try expectPath("/system.slice/system-getty.slice/getty@tty1.service", rule_system_service, .system_services, .high, "getty@tty1.service");
    try expectPath("/sys-kernel-debug.mount", rule_no_systemd_service, .system_services, .low, "sys-kernel-debug.mount");
    try expectPath(user_manager ++ "/app.slice/app-org.chromium.Chromium-3955.scope", rule_application, .applications, .high, "org.chromium.Chromium");
    try expectPath(user_manager ++ "/app.slice/ram-tool.service", rule_user_service, .session_services, .medium, "ram-tool.service");
    try expectPath(user_manager ++ "/app.slice/app-org.gnome.Terminal.slice/vte-spawn-0a1b2c3d.scope", rule_application, .applications, .medium, "org.gnome.Terminal");
    try expectPath("/user.slice/user-1000.slice/session-3.scope", rule_login_session, .login_session, .medium, "3");
}

// ---- processes ----

fn fakeProcess(pid: u32, ppid: u32, start_ticks: u64, comm_text: []const u8, cgroup_path: ?[]const u8) walk.Process {
    var process: walk.Process = .{
        .pid = pid,
        .start_ticks = start_ticks,
        .comm_bytes = @splat(0),
        .comm_len = @intCast(comm_text.len),
        .state = 'S',
        .ppid = ppid,
        .pgrp = pid,
        .session = pid,
        .tty_nr = 0,
        .flags = 0,
        .utime_ticks = 0,
        .stime_ticks = 0,
        .num_threads = 1,
        .priority = 20,
        .nice = 0,
        .status = null,
        .schedstat = null,
        .io = null,
        .cgroup_path = cgroup_path,
    };
    @memcpy(process.comm_bytes[0..comm_text.len], comm_text);
    return process;
}

fn withUid(process: walk.Process, uid: u32) walk.Process {
    var copy = process;
    copy.status = .{ .uid_real = uid };
    return copy;
}

fn classifyAll(processes: []const walk.Process, out: []Classification) !void {
    try classifyProcesses(testing.allocator, processes, out);
}

const editor_scope = user_manager ++ "/app.slice/app-com.visualstudio.CodeOSS-4247.scope";
const alpha_slice = user_manager ++ "/agents.slice/agents-alpha.slice";

test "kernel threads, pid 2 and pid 1 are classed by what they are, whatever their cgroup says" {
    var thread = fakeProcess(77, 2, 5, "kworker/1:0", "/");
    thread.flags = 0x0020_0000;
    const processes = [_]walk.Process{
        fakeProcess(1, 0, 1, "systemd", "/init.scope"),
        fakeProcess(2, 0, 1, "kthreadd", null),
        thread,
        fakeProcess(88, 1, 6, "odd", "/init.scope"),
    };
    var out: [4]Classification = undefined;
    try classifyAll(&processes, &out);
    try testing.expectEqual(@as(u8, rule_init), out[0].rule);
    try testing.expectEqual(Class.kernel, out[1].class);
    try testing.expectEqual(Class.kernel, out[2].class);
    try testing.expectEqual(Confidence.high, out[2].confidence);
    // Not pid 1, but its path says init.
    try testing.expectEqual(@as(u8, rule_init), out[3].rule);
}

test "a process with no cgroup information is placed by its uid and terminal, and never guessed" {
    var login = withUid(fakeProcess(10, 1, 5, "bash", null), 0);
    login.tty_nr = 34816;
    var root_cgroup_login = withUid(fakeProcess(11, 1, 5, "bash", "/"), 1000);
    root_cgroup_login.tty_nr = 34817;
    const processes = [_]walk.Process{
        withUid(fakeProcess(20, 1, 5, "cupsd", null), 0), // system, no terminal
        login, // root shell on a terminal
        root_cgroup_login, // "/" tells nothing either
        withUid(fakeProcess(21, 1, 5, "mystery", null), 1000), // user, no terminal: unknown
        fakeProcess(22, 1, 5, "nostatus", null), // uid unknown: not guessed
        withUid(fakeProcess(23, 1, 5, "rootdaemon", "/"), 0),
    };
    var out: [6]Classification = undefined;
    try classifyAll(&processes, &out);
    try testing.expectEqual(@as(u8, rule_no_cgroup_system), out[0].rule);
    try testing.expectEqual(Class.system_services, out[0].class);
    try testing.expectEqual(Confidence.low, out[0].confidence);
    try testing.expectEqual(@as(u8, rule_no_cgroup_login), out[1].rule);
    try testing.expectEqual(@as(u8, rule_no_cgroup_login), out[2].rule);
    try testing.expectEqual(Class.unclassified, out[3].class);
    try testing.expectEqual(Class.unclassified, out[4].class);
    try testing.expectEqual(@as(u8, rule_no_cgroup_system), out[5].rule);
}

test "agent sessions, jobs and an orphaned agent shell are exact by their scope" {
    const processes = [_]walk.Process{
        fakeProcess(100, 50, 100, "claude", alpha_slice ++ "/claude-alpha-100.scope"),
        fakeProcess(101, 100, 101, "bash", alpha_slice ++ "/claude-alpha-100.scope"),
        fakeProcess(200, 101, 200, "ninja", alpha_slice ++ "/job-alpha-200.scope"),
        // Reparented to pid 1 but still in its scope.
        fakeProcess(300, 1, 300, "bash", user_manager ++ "/agents.slice/agents-beta.slice/opencode-beta-300.scope"),
    };
    var out: [4]Classification = undefined;
    try classifyAll(&processes, &out);
    for (out[0..2]) |got| {
        try testing.expectEqual(@as(u8, rule_agent_session_scope), got.rule);
        try testing.expectEqualStrings("claude-alpha-100.scope", got.key);
        try testing.expectEqual(Confidence.high, got.confidence);
    }
    try testing.expectEqual(@as(u8, rule_agent_job_scope), out[2].rule);
    try testing.expectEqualStrings("job-alpha-200.scope", out[2].key);
    try testing.expectEqual(@as(u8, rule_agent_session_scope), out[3].rule);
    try testing.expectEqualStrings("opencode-beta-300.scope", out[3].key);
}

test "in a scope shared with an editor only the agent's descendants become an agent session" {
    const processes = [_]walk.Process{
        fakeProcess(400, 1, 400, "codium", editor_scope),
        fakeProcess(401, 400, 401, "codium", editor_scope), // editor helper
        fakeProcess(410, 400, 410, "bash", editor_scope), // the terminal, before the agent
        fakeProcess(411, 410, 500, "claude", editor_scope),
        fakeProcess(412, 411, 501, "node", editor_scope),
        fakeProcess(413, 412, 502, "claude", editor_scope), // a nested `claude -p`
        fakeProcess(414, 413, 503, "bash", editor_scope),
        fakeProcess(420, 400, 510, "git", editor_scope), // started by the editor after the agent
    };
    var out: [8]Classification = undefined;
    try classifyAll(&processes, &out);
    for ([_]usize{ 0, 1, 2, 7 }) |index| {
        try testing.expectEqual(@as(u8, rule_application), out[index].rule);
        try testing.expectEqualStrings("com.visualstudio.CodeOSS", out[index].key);
    }
    for ([_]usize{ 3, 4, 5, 6 }) |index| {
        try testing.expectEqual(@as(u8, rule_agent_ancestry), out[index].rule);
        try testing.expectEqual(Class.agent_sessions, out[index].class);
        try testing.expectEqual(Confidence.medium, out[index].confidence);
        try testing.expectEqual(KeyKind.agent_process, out[index].key_kind);
        // The outermost agent, so the nested one counts toward the session that started it.
        try testing.expectEqual(@as(u32, 411), out[index].agent_pid);
        try testing.expectEqual(@as(u64, 500), out[index].agent_start_ticks);
        try testing.expectEqualStrings("claude", out[index].key);
    }
}

test "an agent's descendants in a login session or a user service are refined, those in a system service are not" {
    const processes = [_]walk.Process{
        fakeProcess(600, 1, 100, "pi", "/user.slice/user-1000.slice/session-c2.scope"),
        fakeProcess(601, 600, 101, "sh", "/user.slice/user-1000.slice/session-c2.scope"),
        fakeProcess(610, 1, 100, "opencode", user_manager ++ "/app.slice/helper.service"),
        fakeProcess(611, 610, 101, "sh", user_manager ++ "/app.slice/helper.service"),
        fakeProcess(620, 1, 100, "claude", "/system.slice/runner.service"),
        fakeProcess(621, 620, 101, "sh", "/system.slice/runner.service"),
    };
    var out: [6]Classification = undefined;
    try classifyAll(&processes, &out);
    for (out[0..4]) |got| try testing.expectEqual(@as(u8, rule_agent_ancestry), got.rule);
    for (out[4..6]) |got| try testing.expectEqual(@as(u8, rule_system_service), got.rule);
}

test "a parent-pid cycle, a self-parent and a reused pid end the climb" {
    const processes = [_]walk.Process{
        // A cycle of two with equal start times, one of them an agent.
        fakeProcess(700, 701, 100, "claude", editor_scope),
        fakeProcess(701, 700, 100, "sh", editor_scope),
        // A process that is its own parent.
        fakeProcess(710, 710, 100, "sh", editor_scope),
        // A child whose "parent" pid belongs to a process that started later: a reused pid.
        fakeProcess(720, 1, 900, "claude", editor_scope),
        fakeProcess(721, 720, 500, "sh", editor_scope),
    };
    var out: [5]Classification = undefined;
    try classifyAll(&processes, &out);
    try testing.expectEqual(@as(u8, rule_agent_ancestry), out[0].rule);
    // The other member of the cycle is classified too; which side of the loop
    // counts as the ancestor depends on where the climb entered, and only the
    // ending of the climb is promised.
    try testing.expect(out[1].class == .applications or out[1].class == .agent_sessions);
    try testing.expectEqual(@as(u8, rule_application), out[2].rule);
    try testing.expectEqual(@as(u8, rule_agent_ancestry), out[3].rule);
    try testing.expectEqual(@as(u8, rule_application), out[4].rule);
}

test "a process in a node that holds several classes is unclassified, never mixed, even beside an agent" {
    const processes = [_]walk.Process{
        fakeProcess(30, 1, 1, "claude", user_manager ++ "/app.slice"),
        fakeProcess(31, 30, 2, "sh", user_manager),
        fakeProcess(32, 1, 3, "sh", "/user.slice"),
    };
    var out: [3]Classification = undefined;
    try classifyAll(&processes, &out);
    for (out) |got| {
        try testing.expectEqual(Class.unclassified, got.class);
        try testing.expect(!got.mixed);
        try testing.expectEqual(@as(u8, rule_otherwise), got.rule);
    }
}

test "a mismatched output slice is refused" {
    const processes = [_]walk.Process{fakeProcess(1, 0, 1, "systemd", null)};
    var out: [2]Classification = undefined;
    try testing.expectError(error.LengthMismatch, classifyProcesses(testing.allocator, &processes, &out));
}

test "an empty snapshot classifies to nothing" {
    var out: [0]Classification = undefined;
    try classifyProcesses(testing.allocator, &.{}, &out);
}

test "a failing allocation is reported and leaks nothing" {
    const processes = [_]walk.Process{fakeProcess(5, 1, 1, "claude", editor_scope)};
    var out: [1]Classification = undefined;
    var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = 0 });
    try testing.expectError(error.OutOfMemory, classifyProcesses(failing.allocator(), &processes, &out));
    failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = 1 });
    try testing.expectError(error.OutOfMemory, classifyProcesses(failing.allocator(), &processes, &out));
}

test "the fixture walk is classified: pid 1 init, the kernel threads kernel, the own-user processes by their cgroup" {
    const io = testing.io;
    var root = try std.Io.Dir.cwd().openDir(io, "testdata/proc", .{ .iterate = true });
    defer root.close(io);
    var snapshot = walk.Snapshot.init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(io, root);

    const out = try testing.allocator.alloc(Classification, snapshot.processes.items.len);
    defer testing.allocator.free(out);
    try classifyProcesses(testing.allocator, snapshot.processes.items, out);
    for (snapshot.processes.items, out) |process, got| {
        errdefer std.debug.print("pid {d} {s} path {?s}\n", .{ process.pid, process.comm(), process.cgroup_path });
        try testing.expect(got.class != .unclassified);
        if (process.pid == 1) try testing.expectEqual(Class.init, got.class);
        if (process.isKernelThread()) try testing.expectEqual(Class.kernel, got.class);
    }
}

// ---- fuzz ----

fn insideInput(input: []const u8, part: []const u8) bool {
    if (part.len == 0) return true;
    const start = @intFromPtr(part.ptr);
    const base = @intFromPtr(input.ptr);
    return start >= base and start + part.len <= base + input.len;
}

test "fuzz: arbitrary path bytes never panic and the key lies inside the path" {
    try std.testing.fuzz({}, struct {
        fn run(_: void, smith: *std.testing.Smith) anyerror!void {
            var input: [200]u8 = undefined;
            const length = smith.slice(&input);
            const path = input[0..length];
            const got = classifyPath(path);
            try testing.expect(insideInput(path, got.key));
            try testing.expect(got.rule < rules.len);
            try testing.expectEqual(rules[got.rule].class, got.class);
            if (got.class == .unclassified) try testing.expectEqual(Confidence.none, got.confidence);
            _ = parseAppId(path);
            _ = parseAgentScope(path);
        }
    }.run, .{});
}

test "fuzz: arbitrary process tables classify every process, with keys inside their inputs" {
    try std.testing.fuzz({}, struct {
        fn run(_: void, smith: *std.testing.Smith) anyerror!void {
            const max_processes = 12;
            const paths = [_]?[]const u8{
                null,
                "/",
                "/init.scope",
                editor_scope,
                alpha_slice ++ "/claude-alpha-9.scope",
                "/system.slice/ssh.service",
                "/user.slice/user-1000.slice/session-c2.scope",
            };
            const comms = [_][]const u8{ "claude", "opencode", "pi", "bash", "node", "" };

            var processes: [max_processes]walk.Process = undefined;
            const count = smith.valueRangeAtMost(u8, 0, max_processes);
            for (processes[0..count]) |*process| {
                const comm_choice = smith.valueRangeAtMost(u8, 0, comms.len);
                var comm_bytes: [8]u8 = undefined;
                const comm_length = smith.slice(&comm_bytes);
                // A named agent program, or arbitrary bytes.
                const comm_text = if (comm_choice < comms.len) comms[comm_choice] else comm_bytes[0..comm_length];
                const path_choice = smith.valueRangeAtMost(u8, 0, paths.len - 1);
                process.* = fakeProcess(
                    smith.valueRangeAtMost(u32, 0, 16),
                    smith.valueRangeAtMost(u32, 0, 16),
                    smith.valueRangeAtMost(u64, 0, 4),
                    comm_text,
                    paths[path_choice],
                );
                process.flags = if (smith.value(bool)) 0x0020_0000 else 0;
                process.tty_nr = if (smith.value(bool)) 0 else 1;
                if (smith.value(bool)) process.status = .{ .uid_real = smith.value(u32) };
            }
            var out: [max_processes]Classification = undefined;
            try classifyProcesses(testing.allocator, processes[0..count], out[0..count]);

            for (processes[0..count], out[0..count]) |process, got| {
                try testing.expect(got.rule < rules.len);
                try testing.expectEqual(rules[got.rule].class, got.class);
                if (process.isKernelThread()) try testing.expectEqual(Class.kernel, got.class);
                if (got.key_kind == .agent_process) {
                    // The agent named is a process of the table with that identity.
                    var found = false;
                    for (processes[0..count]) |other| {
                        if (other.pid == got.agent_pid and other.start_ticks == got.agent_start_ticks) found = true;
                    }
                    try testing.expect(found);
                    try testing.expect(isAgentComm(got.key));
                } else if (got.key.len > 0) {
                    try testing.expect(insideInput(process.cgroup_path.?, got.key));
                }
            }
        }
    }.run, .{});
}

test "allocation budget: classifying a snapshot with agents makes two allocations and none without" {
    var table: [40]walk.Process = undefined;
    for (&table, 0..) |*process, index| {
        process.* = fakeProcess(@intCast(100 + index), 1, 10, "bash", editor_scope);
    }
    table[7] = fakeProcess(107, 1, 10, "claude", editor_scope);
    var out: [40]Classification = undefined;

    var counting = testing.FailingAllocator.init(testing.allocator, .{});
    try classifyProcesses(counting.allocator(), &table, &out);
    try testing.expectEqual(@as(usize, 2), counting.allocations);

    table[7] = fakeProcess(107, 1, 10, "bash", editor_scope);
    counting = testing.FailingAllocator.init(testing.allocator, .{});
    try classifyProcesses(counting.allocator(), &table, &out);
    try testing.expectEqual(@as(usize, 0), counting.allocations);
}

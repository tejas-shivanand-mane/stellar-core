// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{
        CxxBuf, CxxFeeConfiguration, CxxLedgerEntryRentChange, CxxLedgerInfo,
        CxxRentFeeConfiguration, CxxRentWriteFeeConfiguration, CxxTransactionResources, FeePair,
        InvokeHostFunctionOutput, RustBuf, SorobanVersionInfo, XDRFileHash,
    },
};
use log::{debug, error, trace, warn};
use std::{fmt::Display, io::Cursor, panic, rc::Rc, time::Instant};

// This module (soroban_proto_any) is bound to _multiple locations_ in the
// module tree of this crate:
//
//    crate::soroban_proto_all::p21::soroban_proto_any
//    crate::soroban_proto_all::p22::soroban_proto_any
//    crate::soroban_proto_all::p23::soroban_proto_any ...
//
// Each such location is embedded inside a parent module -- p21, p22, p23, etc.
// -- which is an adaptor for a specific version of soroban: it provides a
// specific soroban version binding for `soroban_env_host` which we import here
// from the adaptor: we refer to `super::soroban_env_host`, rather than
// referring to any extern crate directly.
//
// Consequently the code in this module will be interpreted _simultaneously_ by
// the compiler as referring to each of the different soroban_env_host crates.
// In other words the code in this module has to work with any of them, or in
// yet other words the soroban interface you get to talk to here is the
// intersection of any/all the sorobans linked into this crate. If you cannot
// write code that is "version agnostic" in this way, you need to write some
// adaptor code (that differs for each version), stick it in _all_ the p21, p22,
// ... adaptor modules that this module is mounted inside of, and then import
// the adaptor from there to here.
//
// The point of all this muddle is to allow us to have a mostly-singular set of
// functions in this file, rather than maintaining a separate copy for each
// version of soroban_env_host. But it does mean that you need to be careful
// about what you do here, and sometimes compensate with indirection through the
// outer adaptor modules.
pub(crate) use super::soroban_env_host::{
    budget::{AsBudget, Budget},
    e2e_invoke::{extract_rent_changes, LedgerEntryChange},
    fees::{
        compute_rent_fee as host_compute_rent_fee,
        compute_transaction_resource_fee as host_compute_transaction_resource_fee,
        FeeConfiguration, LedgerEntryRentChange, RentFeeConfiguration, TransactionResources,
    },
    xdr::{
        self, ContractCodeEntry, ContractCostParams, ContractEvent, ContractEventBody,
        ContractEventType, ContractEventV0, DiagnosticEvent, ExtensionPoint, LedgerEntry,
        LedgerEntryData, LedgerEntryExt, Limits, ReadXdr, ScError, ScErrorCode, ScErrorType,
        ScSymbol, ScVal, TransactionEnvelope, TtlEntry, WriteXdr, XDR_FILES_SHA256,
    },
    HostError, LedgerInfo, Val, VERSION,
};
use super::{ErrorHandler, ModuleCache};
use std::error::Error;

impl TryFrom<&CxxLedgerInfo> for LedgerInfo {
    type Error = Box<dyn Error>;
    fn try_from(c: &CxxLedgerInfo) -> Result<Self, Self::Error> {
        Ok(Self {
            protocol_version: c.protocol_version,
            sequence_number: c.sequence_number,
            timestamp: c.timestamp,
            network_id: c.network_id.clone().try_into().map_err(|_| {
                Box::new(CoreHostError::General("network ID has wrong size".into()))
            })?,
            base_reserve: c.base_reserve,
            min_temp_entry_ttl: c.min_temp_entry_ttl,
            min_persistent_entry_ttl: c.min_persistent_entry_ttl,
            max_entry_ttl: c.max_entry_ttl,
        })
    }
}

impl From<CxxTransactionResources> for TransactionResources {
    fn from(value: CxxTransactionResources) -> Self {
        super::convert_transaction_resources(&value)
    }
}

impl From<CxxFeeConfiguration> for FeeConfiguration {
    fn from(value: CxxFeeConfiguration) -> Self {
        super::convert_fee_configuration(value)
    }
}

impl From<&CxxLedgerEntryRentChange> for LedgerEntryRentChange {
    fn from(value: &CxxLedgerEntryRentChange) -> Self {
        super::convert_ledger_entry_rent_change(value)
    }
}

impl From<&CxxRentFeeConfiguration> for RentFeeConfiguration {
    fn from(value: &CxxRentFeeConfiguration) -> Self {
        super::convert_rent_fee_configuration(value)
    }
}

// FIXME: plumb this through from the limit xdrpp uses.
// Currently they are just two same-valued constants.
const MARSHALLING_STACK_LIMIT: u32 = 1000;

#[allow(dead_code)]
#[derive(Debug)]
pub(crate) enum CoreHostError {
    Host(HostError),
    General(String),
}

impl Display for CoreHostError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl From<HostError> for CoreHostError {
    fn from(h: HostError) -> Self {
        CoreHostError::Host(h)
    }
}

impl From<xdr::Error> for CoreHostError {
    fn from(_: xdr::Error) -> Self {
        CoreHostError::Host((ScErrorType::Value, ScErrorCode::InvalidInput).into())
    }
}

impl std::error::Error for CoreHostError {}

fn non_metered_xdr_from_cxx_buf<T: ReadXdr>(buf: &CxxBuf) -> Result<T, HostError> {
    Ok(T::read_xdr(&mut xdr::Limited::new(
        Cursor::new(buf.data.as_slice()),
        Limits {
            depth: MARSHALLING_STACK_LIMIT,
            len: buf.data.len(),
        },
    ))
    // We only expect this to be called for safe, internal conversions, so this
    // should never happen.
    .map_err(|_| (ScErrorType::Value, ScErrorCode::InternalError))?)
}

fn non_metered_xdr_to_vec<T: WriteXdr>(t: &T) -> Result<Vec<u8>, HostError> {
    let mut vec: Vec<u8> = Vec::new();
    t.write_xdr(&mut xdr::Limited::new(
        Cursor::new(&mut vec),
        Limits {
            depth: MARSHALLING_STACK_LIMIT,
            len: 5 * 1024 * 1024, /* 5MB */
        },
    ))
    .map_err(|_| (ScErrorType::Value, ScErrorCode::InvalidInput))?;
    Ok(vec)
}

fn non_metered_xdr_to_rust_buf<T: WriteXdr>(t: &T) -> Result<RustBuf, HostError> {
    Ok(RustBuf {
        data: non_metered_xdr_to_vec(t)?,
    })
}

// This is just a helper for modifying some data that is encoded in a CxxBuf. It
// decodes the data, modifies it, and then re-encodes it back into the CxxBuf.
// It's intended for use when modifying the cost parameters of the CxxLedgerInfo
// when invoking a contract twice with different protocols.
#[allow(dead_code)]
#[cfg(feature = "testutils")]
pub(crate) fn inplace_modify_cxxbuf_encoded_type<T: ReadXdr + WriteXdr>(
    buf: &mut CxxBuf,
    modify: impl FnOnce(&mut T) -> Result<(), Box<dyn Error>>,
) -> Result<(), Box<dyn Error>> {
    let mut tmp = non_metered_xdr_from_cxx_buf::<T>(buf)?;
    modify(&mut tmp)?;
    let vec = non_metered_xdr_to_vec::<T>(&tmp)?;
    buf.replace_data_with(vec.as_slice())
}

/// Returns a vec of [`XDRFileHash`] structs each representing one .x file
/// that served as input to xdrgen, which created the XDR definitions used in
/// the Rust crates visible here. This allows the C++ side of the bridge
/// to confirm that the same definitions are compiled into the C++ code.
pub fn get_xdr_hashes() -> Vec<XDRFileHash> {
    XDR_FILES_SHA256
        .iter()
        .map(|(file, hash)| XDRFileHash {
            file: (*file).into(),
            hash: (*hash).into(),
        })
        .collect()
}

pub const fn get_max_proto() -> u32 {
    super::get_version_protocol(&VERSION)
}

pub fn get_soroban_version_info(core_max_proto: u32) -> SorobanVersionInfo {
    let env_max_proto = get_max_proto();
    let xdr_base_git_rev = match VERSION.xdr.xdr {
        "curr" => VERSION.xdr.xdr_curr.to_string(),
        "next" | "curr,next" => {
            if !cfg!(feature = "next") {
                warn!(
                    "soroban version {} XDR module built with 'next' feature,
                       but core built without 'vnext' feature",
                    VERSION.pkg
                );
            }
            if core_max_proto != env_max_proto {
                warn!(
                    "soroban version {} XDR module for env version {} built with 'next' feature, \
                       even though this is not the newest core protocol ({})",
                    VERSION.pkg, env_max_proto, core_max_proto
                );
                warn!(
                    "this can happen if multiple soroban crates depend on the \
                       same XDR crate which then gets feature-unified"
                )
            }
            VERSION.xdr.xdr_next.to_string()
        }
        other => format!("unknown XDR module configuration: '{other}'"),
    };

    SorobanVersionInfo {
        env_max_proto,
        env_pkg_ver: VERSION.pkg.to_string(),
        env_git_rev: VERSION.rev.to_string(),
        env_pre_release_ver: super::get_version_pre_release(&VERSION),
        xdr_pkg_ver: VERSION.xdr.pkg.to_string(),
        xdr_git_rev: VERSION.xdr.rev.to_string(),
        xdr_base_git_rev,
        xdr_file_hashes: get_xdr_hashes(),
    }
}

fn log_diagnostic_events(events: &Vec<DiagnosticEvent>) {
    for e in events {
        debug!("Diagnostic event: {:?}", e);
    }
}

fn encode_diagnostic_events(events: &Vec<DiagnosticEvent>) -> Vec<RustBuf> {
    events
        .iter()
        .filter_map(|e| {
            if let Ok(encoded) = non_metered_xdr_to_rust_buf(e) {
                Some(encoded)
            } else {
                None
            }
        })
        .collect()
}

fn extract_ledger_effects(
    entry_changes: Vec<LedgerEntryChange>,
) -> Result<Vec<RustBuf>, HostError> {
    let mut modified_entries = vec![];

    for change in entry_changes {
        // Extract ContractCode and ContractData entry changes first
        if !change.read_only {
            if let Some(encoded_new_value) = change.encoded_new_value {
                modified_entries.push(encoded_new_value.into());
            }
        }

        // Check for TtlEntry changes
        if let Some(ttl_change) = change.ttl_change {
            if ttl_change.new_live_until_ledger > ttl_change.old_live_until_ledger {
                // entry_changes only encode LedgerEntry changes for ContractCode and ContractData
                // entries. Changes to TtlEntry are recorded in ttl_change, but does
                // not contain an encoded TtlEntry. We must build that here.
                let hash_bytes: [u8; 32] = ttl_change
                    .key_hash
                    .try_into()
                    .map_err(|_| (ScErrorType::Value, ScErrorCode::InternalError))?;

                let le = LedgerEntry {
                    last_modified_ledger_seq: 0,
                    data: LedgerEntryData::Ttl(TtlEntry {
                        key_hash: hash_bytes.into(),
                        live_until_ledger_seq: ttl_change.new_live_until_ledger,
                    }),
                    ext: LedgerEntryExt::V0,
                };

                let encoded = non_metered_xdr_to_rust_buf(&le)
                    .map_err(|_| (ScErrorType::Value, ScErrorCode::InternalError))?;
                modified_entries.push(encoded);
            }
        }
    }

    Ok(modified_entries)
}

/// Deserializes an [`xdr::HostFunction`] host function XDR object an
/// [`xdr::Footprint`] and a sequence of [`xdr::LedgerEntry`] entries containing all
/// the data the invocation intends to read. Then calls the specified host function
/// and returns the [`InvokeHostFunctionOutput`] that contains the host function
/// result, events and modified ledger entries. Ledger entries not returned have
/// been deleted.
pub(crate) fn invoke_host_function(
    enable_diagnostics: bool,
    instruction_limit: u32,
    hf_buf: &CxxBuf,
    resources_buf: &CxxBuf,
    restored_rw_entry_indices: &Vec<u32>,
    source_account_buf: &CxxBuf,
    auth_entries: &Vec<CxxBuf>,
    ledger_info: &CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    ttl_entries: &Vec<CxxBuf>,
    base_prng_seed: &CxxBuf,
    rent_fee_configuration: &CxxRentFeeConfiguration,
    module_cache: &crate::SorobanModuleCache,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let res = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        invoke_host_function_or_maybe_panic(
            enable_diagnostics,
            instruction_limit,
            hf_buf,
            resources_buf,
            restored_rw_entry_indices.as_slice(),
            source_account_buf,
            auth_entries,
            ledger_info,
            ledger_entries,
            ttl_entries,
            base_prng_seed,
            rent_fee_configuration,
            module_cache,
        )
    }));
    match res {
        Err(r) => {
            if let Some(s) = r.downcast_ref::<String>() {
                Err(CoreHostError::General(format!("contract host panicked: {s}")).into())
            } else if let Some(s) = r.downcast_ref::<&'static str>() {
                Err(CoreHostError::General(format!("contract host panicked: {s}")).into())
            } else {
                Err(CoreHostError::General("contract host panicked".into()).into())
            }
        }
        Ok(r) => r,
    }
}

fn make_trace_hook_fn<'a>() -> super::soroban_env_host::TraceHook {
    let prev_state = std::cell::RefCell::new(String::new());
    Rc::new(move |host, traceevent| {
        if traceevent.is_begin() || traceevent.is_end() {
            prev_state.replace(String::new());
        }
        match super::soroban_env_host::TraceRecord::new(host, traceevent) {
            Ok(tr) => {
                let state_str = format!("{}", tr.state);
                if prev_state.borrow().is_empty() {
                    trace!(target: TX, "{}: {}", tr.event, state_str);
                } else {
                    let diff = crate::log::diff_line(&prev_state.borrow(), &state_str);
                    trace!(target: TX, "{}: {}", tr.event, diff);
                }
                prev_state.replace(state_str);
            }
            Err(e) => trace!(target: TX, "{}", e),
        }
        Ok(())
    })
}

#[allow(dead_code)]
#[cfg(feature = "testutils")]
fn decode_contract_cost_params(buf: &CxxBuf) -> Result<ContractCostParams, Box<dyn Error>> {
    Ok(non_metered_xdr_from_cxx_buf::<ContractCostParams>(buf)?)
}

#[allow(dead_code)]
#[cfg(feature = "testutils")]
fn encode_contract_cost_params(params: &ContractCostParams) -> Result<RustBuf, Box<dyn Error>> {
    Ok(non_metered_xdr_to_rust_buf(params)?)
}

fn invoke_host_function_or_maybe_panic(
    enable_diagnostics: bool,
    instruction_limit: u32,
    hf_buf: &CxxBuf,
    resources_buf: &CxxBuf,
    restored_rw_entry_indices: &[u32],
    source_account_buf: &CxxBuf,
    auth_entries: &Vec<CxxBuf>,
    ledger_info: &CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    ttl_entries: &Vec<CxxBuf>,
    base_prng_seed: &CxxBuf,
    rent_fee_configuration: &CxxRentFeeConfiguration,
    module_cache: &crate::SorobanModuleCache,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    #[cfg(feature = "tracy")]
    let client = tracy_client::Client::start();
    let _span0 = tracy_span!("invoke_host_function_or_maybe_panic");

    let protocol_version = ledger_info.protocol_version;

    let budget = Budget::try_from_configs(
        instruction_limit as u64,
        ledger_info.memory_limit as u64,
        // These are the only non-metered XDR conversions that we perform. They
        // have a small constant cost that is independent of the user-provided
        // data.
        non_metered_xdr_from_cxx_buf::<ContractCostParams>(&ledger_info.cpu_cost_params)?,
        non_metered_xdr_from_cxx_buf::<ContractCostParams>(&ledger_info.mem_cost_params)?,
    )?;
    let mut diagnostic_events = vec![];
    let ledger_seq_num = ledger_info.sequence_number;
    let trace_hook: Option<super::soroban_env_host::TraceHook> =
        if crate::log::is_tx_tracing_enabled() {
            Some(make_trace_hook_fn())
        } else {
            None
        };
    let (res, time_nsecs) = {
        let _span1 = tracy_span!("e2e_invoke::invoke_function");
        let start_time = Instant::now();

        let res = super::invoke_host_function_with_trace_hook_and_module_cache(
            &budget,
            enable_diagnostics,
            hf_buf,
            resources_buf,
            restored_rw_entry_indices,
            source_account_buf,
            auth_entries.iter(),
            ledger_info.try_into()?,
            ledger_entries.iter(),
            ttl_entries.iter(),
            base_prng_seed,
            &mut diagnostic_events,
            trace_hook,
            module_cache,
        );
        let stop_time = Instant::now();
        let time_nsecs = stop_time.duration_since(start_time).as_nanos() as u64;
        (res, time_nsecs)
    };

    // Unconditionally log diagnostic events (there won't be any if diagnostics
    // is disabled).
    log_diagnostic_events(&diagnostic_events);

    let cpu_insns = budget.get_cpu_insns_consumed()?;
    let mem_bytes = budget.get_mem_bytes_consumed()?;
    let cpu_insns_excluding_vm_instantiation = cpu_insns.saturating_sub(
        budget
            .get_tracker(xdr::ContractCostType::VmInstantiation)?
            .cpu,
    );
    let time_nsecs_excluding_vm_instantiation =
        time_nsecs.saturating_sub(budget.get_time(xdr::ContractCostType::VmInstantiation)?);
    #[cfg(feature = "tracy")]
    {
        client.plot(
            tracy_client::plot_name!("soroban budget cpu"),
            cpu_insns as f64,
        );
        client.plot(
            tracy_client::plot_name!("soroban budget mem"),
            mem_bytes as f64,
        );
    }
    let err = match res {
        Ok(res) => match res.encoded_invoke_result {
            Ok(result_value) => {
                let rent_changes = extract_rent_changes(&res.ledger_changes);
                let rent_fee = host_compute_rent_fee(
                    &rent_changes,
                    &rent_fee_configuration.into(),
                    ledger_seq_num,
                );
                let modified_ledger_entries = extract_ledger_effects(res.ledger_changes)?;
                return Ok(InvokeHostFunctionOutput {
                    success: true,
                    is_internal_error: false,
                    diagnostic_events: encode_diagnostic_events(&diagnostic_events),
                    cpu_insns,
                    mem_bytes,
                    time_nsecs,
                    cpu_insns_excluding_vm_instantiation,
                    time_nsecs_excluding_vm_instantiation,

                    result_value: result_value.into(),
                    modified_ledger_entries,
                    contract_events: res
                        .encoded_contract_events
                        .into_iter()
                        .map(RustBuf::from)
                        .collect(),
                    rent_fee,
                });
            }
            Err(e) => e,
        },
        Err(e) => e,
    };
    if enable_diagnostics {
        diagnostic_events.push(DiagnosticEvent {
            in_successful_contract_call: false,
            event: ContractEvent {
                ext: ExtensionPoint::V0,
                contract_id: None,
                type_: ContractEventType::Diagnostic,
                body: ContractEventBody::V0(ContractEventV0 {
                    topics: vec![
                        ScVal::Symbol(ScSymbol("host_fn_failed".try_into().unwrap_or_default())),
                        ScVal::Error(
                            err.error
                                .try_into()
                                .unwrap_or(ScError::Context(ScErrorCode::InternalError)),
                        ),
                    ]
                    .try_into()
                    .unwrap_or_default(),
                    data: ScVal::Void,
                }),
            },
        })
    }
    let is_internal_error = if protocol_version < 22 {
        err.error.is_code(ScErrorCode::InternalError)
    } else {
        err.error.is_code(ScErrorCode::InternalError) && !err.error.is_type(ScErrorType::Contract)
    };

    debug!(target: TX, "invocation failed: {}", err);
    return Ok(InvokeHostFunctionOutput {
        success: false,
        is_internal_error,
        diagnostic_events: encode_diagnostic_events(&diagnostic_events),
        cpu_insns,
        mem_bytes,
        time_nsecs,
        cpu_insns_excluding_vm_instantiation,
        time_nsecs_excluding_vm_instantiation,

        result_value: vec![].into(),
        modified_ledger_entries: vec![],
        contract_events: vec![],
        rent_fee: 0,
    });
}

#[allow(dead_code)]
#[cfg(feature = "testutils")]
pub(crate) fn rustbuf_containing_scval_to_string(buf: &RustBuf) -> String {
    if let Ok(val) = ScVal::read_xdr(&mut xdr::Limited::new(
        Cursor::new(buf.data.as_slice()),
        Limits {
            depth: MARSHALLING_STACK_LIMIT,
            len: buf.data.len(),
        },
    )) {
        format!("{:?}", val)
    } else {
        "<bad ScVal>".to_string()
    }
}

#[allow(dead_code)]
#[cfg(feature = "testutils")]
pub(crate) fn rustbuf_containing_diagnostic_event_to_string(buf: &RustBuf) -> String {
    if let Ok(val) = DiagnosticEvent::read_xdr(&mut xdr::Limited::new(
        Cursor::new(buf.data.as_slice()),
        Limits {
            depth: MARSHALLING_STACK_LIMIT,
            len: buf.data.len(),
        },
    )) {
        format!("{:?}", val)
    } else {
        "<bad DiagnosticEvent>".to_string()
    }
}

pub(crate) fn compute_transaction_resource_fee(
    tx_resources: CxxTransactionResources,
    fee_config: CxxFeeConfiguration,
) -> FeePair {
    let (non_refundable_fee, refundable_fee) =
        host_compute_transaction_resource_fee(&tx_resources.into(), &fee_config.into());
    FeePair {
        non_refundable_fee,
        refundable_fee,
    }
}

pub(crate) fn compute_rent_fee(
    changed_entries: &Vec<CxxLedgerEntryRentChange>,
    fee_config: CxxRentFeeConfiguration,
    current_ledger_seq: u32,
) -> i64 {
    let changed_entries: Vec<_> = changed_entries.iter().map(|e| e.into()).collect();
    host_compute_rent_fee(
        &changed_entries,
        &((&fee_config).into()),
        current_ledger_seq,
    )
}

pub(crate) fn compute_rent_write_fee_per_1kb(
    bucket_list_size: i64,
    fee_config: CxxRentWriteFeeConfiguration,
) -> i64 {
    super::compute_rent_write_fee_per_1kb_wrapper(bucket_list_size, fee_config)
}

pub(crate) fn contract_code_memory_size_for_rent(
    contract_code_entry_xdr: &CxxBuf,
    cpu_cost_params: &CxxBuf,
    mem_cost_params: &CxxBuf,
) -> Result<u32, Box<dyn std::error::Error>> {
    let contract_code_entry =
        non_metered_xdr_from_cxx_buf::<ContractCodeEntry>(contract_code_entry_xdr)?;
    let budget = Budget::try_from_configs(
        0,
        0,
        non_metered_xdr_from_cxx_buf::<ContractCostParams>(cpu_cost_params)?,
        non_metered_xdr_from_cxx_buf::<ContractCostParams>(mem_cost_params)?,
    )?;
    super::wasm_module_memory_cost_wrapper(&budget, &contract_code_entry)?
        .try_into()
        .map_err(Into::into)
}

pub(crate) fn can_parse_transaction(xdr: &CxxBuf, depth_limit: u32) -> bool {
    let res = TransactionEnvelope::read_xdr(&mut xdr::Limited::new(
        Cursor::new(xdr.data.as_slice()),
        Limits {
            depth: depth_limit,
            len: xdr.data.len(),
        },
    ));
    res.is_ok()
}

#[allow(dead_code)]
#[derive(Clone)]
struct CoreCompilationContext {
    unlimited_budget: Budget,
}

impl super::CompilationContext for CoreCompilationContext {}

#[allow(dead_code)]
impl CoreCompilationContext {
    fn new() -> Result<Self, Box<dyn std::error::Error>> {
        let unlimited_budget = Budget::try_from_configs(
            u64::MAX,
            u64::MAX,
            ContractCostParams(vec![].try_into().unwrap()),
            ContractCostParams(vec![].try_into().unwrap()),
        )?;
        Ok(CoreCompilationContext { unlimited_budget })
    }
}

impl AsBudget for CoreCompilationContext {
    fn as_budget(&self) -> &Budget {
        &self.unlimited_budget
    }
}

impl ErrorHandler for CoreCompilationContext {
    fn map_err<T, E>(&self, res: Result<T, E>) -> Result<T, HostError>
    where
        super::soroban_env_host::Error: From<E>,
        E: core::fmt::Debug,
    {
        match res {
            Ok(t) => Ok(t),
            Err(e) => {
                error!("compiling module: {:?}", e);
                Err(HostError::from(e))
            }
        }
    }

    fn error(&self, error: super::soroban_env_host::Error, msg: &str, _args: &[Val]) -> HostError {
        error!("compiling module: {:?}: {}", error, msg);
        HostError::from(error)
    }
}

#[allow(dead_code)]
pub(crate) struct ProtocolSpecificModuleCache {
    // `ModuleCache` itself is threadsafe -- does its own internal locking -- so
    // it's ok to directly access it from multiple threads.
    pub(crate) module_cache: ModuleCache,
    // `CompilationContext` is _not_  threadsafe (specifically its `Budget` is
    // not) and so rather than reuse a single `CompilationContext` across
    // threads, we make a throwaway `CompilationContext` on each `compile` call,
    // and _copy out_ the memory usage (which we want to publish back to core).
    pub(crate) mem_bytes_consumed: std::sync::atomic::AtomicU64,
}

#[allow(dead_code)]
impl ProtocolSpecificModuleCache {
    pub(crate) fn new() -> Result<Self, Box<dyn std::error::Error>> {
        let compilation_context = CoreCompilationContext::new()?;
        let module_cache = ModuleCache::new(&compilation_context)?;
        Ok(ProtocolSpecificModuleCache {
            module_cache,
            mem_bytes_consumed: std::sync::atomic::AtomicU64::new(0),
        })
    }

    pub(crate) fn compile(&mut self, wasm: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
        let compilation_context = CoreCompilationContext::new()?;
        let res = self.module_cache.parse_and_cache_module_simple(
            &compilation_context,
            get_max_proto(),
            wasm,
        );
        self.mem_bytes_consumed.fetch_add(
            compilation_context
                .unlimited_budget
                .get_mem_bytes_consumed()?,
            std::sync::atomic::Ordering::SeqCst,
        );
        Ok(res?)
    }

    pub(crate) fn evict(&mut self, key: &[u8; 32]) -> Result<(), Box<dyn std::error::Error>> {
        let _ = self.module_cache.remove_module(&key.clone().into())?;
        Ok(())
    }

    pub(crate) fn clear(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        Ok(self.module_cache.clear()?)
    }

    pub(crate) fn contains_module(
        &self,
        key: &[u8; 32],
    ) -> Result<bool, Box<dyn std::error::Error>> {
        Ok(self.module_cache.contains_module(&key.clone().into())?)
    }

    pub(crate) fn get_mem_bytes_consumed(&self) -> Result<u64, Box<dyn std::error::Error>> {
        Ok(self
            .mem_bytes_consumed
            .load(std::sync::atomic::Ordering::SeqCst))
    }

    // This produces a new `SorobanModuleCache` with a separate
    // `CoreCompilationContext` but a clone of the underlying `ModuleCache`, which
    // will (since the module cache is the reusable flavor) actually point to
    // the _same_ underlying threadsafe map of `Module`s and the same associated
    // `Engine` as those that `self` currently points to.
    //
    // This mainly exists to allow cloning a shared-ownership handle to a
    // (threadsafe) ModuleCache to pass to separate C++-launched threads, to
    // allow multithreaded compilation.
    pub(crate) fn shallow_clone(&self) -> Result<Self, Box<dyn std::error::Error>> {
        let mut new = Self::new()?;
        new.module_cache = self.module_cache.clone();
        Ok(new)
    }
}

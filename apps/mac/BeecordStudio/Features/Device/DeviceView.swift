import SwiftUI

/// 设备页（T5 阶段 C 全量）：扫描状态卡 / 信息卡 / 文件区 / 工具行 / 底部整体进度 / 蓝牙面罩 / 空态。
/// 回归约束：无设备源（idle 且非连接中）必须呈现 `device.emptyState`。
struct DeviceView: View {
    @Bindable var viewModel: DeviceViewModel

    var body: some View {
        VStack(spacing: 0) {
            pageHeader
            Group {
                if let reason = viewModel.permissionOverlay {
                    bluetoothOverlay(reason)
                } else if viewModel.isConnected {
                    connectedArea
                } else if viewModel.snapshot.state == .connecting {
                    scanningCard
                } else {
                    disconnectedEmptyState
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .onAppear {
            viewModel.onAppear()
        }
    }

    // MARK: - 头部

    private var pageHeader: some View {
        HStack {
            Text("设备")
                .font(DesignTokens.FontToken.pageTitle)
            Spacer()
        }
        .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
        .padding(.vertical, DesignTokens.Layout.mainPaddingVertical)
    }

    // MARK: - 未连接：扫描中 / 空态 + 配对引导

    private var scanningCard: some View {
        VStack(spacing: DesignTokens.Space.s3) {
            ProgressView()
                .controlSize(.regular)
            Text("正在连接设备…")
                .font(DesignTokens.FontToken.emptyStateTitle)
            Text("请确保设备已进入同步模式（三击电源键进入配对）。")
                .font(DesignTokens.FontToken.body)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var disconnectedEmptyState: some View {
        VStack(spacing: DesignTokens.Space.s4) {
            EmptyStateView(
                systemImage: "applewatch",
                title: "尚未连接设备",
                message: "将 Beecord 设备置于同步模式后连接；长时间未发现请确认设备已进入配对模式（三击电源键）。",
                accessibilityID: "device.emptyState"
            )
            HStack(spacing: DesignTokens.Space.s3) {
                Button("连接") {
                    viewModel.connectTapped()
                }
                .buttonStyle(.borderedProminent)
                .accessibilityIdentifier("device.connectButton")
                Button("重新扫描") {
                    viewModel.connectTapped()
                }
                .accessibilityIdentifier("device.scanButton")
            }
        }
    }

    // MARK: - 已连接

    private var connectedArea: some View {
        VStack(spacing: DesignTokens.Space.s3) {
            if let info = viewModel.snapshot.deviceInfo {
                DeviceInfoCard(
                    info: info,
                    isStorageNearlyFull: viewModel.snapshot.isStorageNearlyFull,
                    onDisconnect: viewModel.disconnectTapped
                )
                .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
            }
            toolbar
                .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
            if viewModel.snapshot.rows.isEmpty {
                EmptyStateView(
                    systemImage: "waveform",
                    title: "设备上没有录音文件",
                    message: "在设备上完成录音后，回到这里同步。",
                    accessibilityID: "device.fileTable.emptyState"
                )
            } else {
                FileTableView(
                    rows: viewModel.snapshot.rows,
                    selectedIDs: viewModel.selectedIDs,
                    onToggle: viewModel.toggleSelection,
                    onRetry: viewModel.retryTapped
                )
                .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
            }
            stateBanner
            if let progress = viewModel.snapshot.progress {
                SyncProgressBar(
                    progress: progress,
                    isCancelling: viewModel.snapshot.state == .cancelling,
                    onCancel: viewModel.cancelTapped
                )
                .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
                .padding(.bottom, DesignTokens.Space.s4)
            }
        }
    }

    private var toolbar: some View {
        HStack(spacing: DesignTokens.Space.s3) {
            Toggle("全选", isOn: Binding(
                get: { viewModel.isSelectAll },
                set: { _ in viewModel.toggleSelectAll() }
            ))
            .toggleStyle(.checkbox)
            .accessibilityIdentifier("device.selectAllCheckbox")
            Button(viewModel.downloadButtonTitle) {
                viewModel.downloadSelectedTapped()
            }
            .disabled(viewModel.selectedIDs.isEmpty)
            .accessibilityIdentifier("device.downloadButton")
            Toggle("自动同步", isOn: Binding(
                get: { viewModel.settings.autoSyncEnabled },
                set: { viewModel.setAutoSync($0) }
            ))
            .toggleStyle(.checkbox)
            .accessibilityIdentifier("device.autoSyncToggle")
            Spacer()
            Button {
                viewModel.refreshTapped()
            } label: {
                Image(systemName: "arrow.clockwise")
                    .font(.system(size: 14))
            }
            .accessibilityLabel("刷新文件列表")
            .accessibilityIdentifier("device.refreshButton")
        }
    }

    /// 连接态提示条：卡片级失败 / 自动重连中。
    @ViewBuilder
    private var stateBanner: some View {
        switch viewModel.snapshot.state {
        case .failed(let message):
            HStack(spacing: DesignTokens.Space.s2) {
                Image(systemName: "exclamationmark.circle.fill")
                Text(message)
            }
            .font(DesignTokens.FontToken.callout)
            .foregroundStyle(DesignTokens.ColorToken.stateDanger)
            .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
        case .reconnecting:
            HStack(spacing: DesignTokens.Space.s2) {
                ProgressView()
                    .controlSize(.mini)
                Text("连接已断开，正在自动重连…")
            }
            .font(DesignTokens.FontToken.callout)
            .foregroundStyle(DesignTokens.ColorToken.textSecondary)
            .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
        default:
            EmptyView()
        }
    }

    // MARK: - 蓝牙权限面罩

    private func bluetoothOverlay(_ reason: DeviceViewModel.PermissionReason) -> some View {
        VStack(spacing: DesignTokens.Space.s3) {
            Image(systemName: "bluetooth")
                .font(.system(size: 40))
                .foregroundStyle(DesignTokens.ColorToken.textTertiary)
            Text(reason.title)
                .font(DesignTokens.FontToken.emptyStateTitle)
            Text(reason.message)
                .font(DesignTokens.FontToken.body)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                .multilineTextAlignment(.center)
            HStack(spacing: DesignTokens.Space.s3) {
                Button("打开系统设置") {
                    viewModel.openBluetoothSettings()
                }
                .buttonStyle(.borderedProminent)
                .accessibilityIdentifier("device.settingsButton")
                Button("重试") {
                    viewModel.connectTapped()
                }
                .accessibilityIdentifier("device.retryButton")
            }
            .padding(.top, DesignTokens.Space.s1)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(DesignTokens.Space.s8)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("device.bluetoothOverlay")
    }
}

#Preview("浅色 · 未连接") {
    DeviceView(viewModel: DeviceViewModel(appModel: AppModel(), settings: SettingsModel()))
}

#Preview("深色 · 未连接") {
    DeviceView(viewModel: DeviceViewModel(appModel: AppModel(), settings: SettingsModel()))
        .preferredColorScheme(.dark)
}

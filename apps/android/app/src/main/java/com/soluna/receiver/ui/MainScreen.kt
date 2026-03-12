package com.soluna.receiver.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.soluna.receiver.SolunaClient

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(client: SolunaClient) {
    val state by client.state.collectAsState()
    val focusManager = LocalFocusManager.current

    var serverAddress by remember { mutableStateOf("relay.solun.art:5100") }
    var channelName by remember { mutableStateOf("default") }
    var tipAmount by remember { mutableStateOf("1.00") }
    var showTipDialog by remember { mutableStateOf(false) }

    val isConnected = state.status == SolunaClient.Status.CONNECTED
    val isConnecting = state.status == SolunaClient.Status.CONNECTING

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text("Soluna Receiver", fontWeight = FontWeight.Bold)
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.primaryContainer
                )
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // --- Connection settings ---
            OutlinedTextField(
                value = serverAddress,
                onValueChange = { serverAddress = it },
                label = { Text("Relay Server (host:port)") },
                singleLine = true,
                enabled = !isConnected && !isConnecting,
                modifier = Modifier.fillMaxWidth(),
                leadingIcon = { Icon(Icons.Default.Dns, contentDescription = null) },
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next)
            )

            OutlinedTextField(
                value = channelName,
                onValueChange = { channelName = it },
                label = { Text("Channel Name") },
                singleLine = true,
                enabled = !isConnected && !isConnecting,
                modifier = Modifier.fillMaxWidth(),
                leadingIcon = { Icon(Icons.Default.Radio, contentDescription = null) },
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { focusManager.clearFocus() })
            )

            // --- Connect / Disconnect button ---
            Button(
                onClick = {
                    focusManager.clearFocus()
                    if (isConnected || isConnecting) {
                        client.disconnect()
                    } else {
                        val parts = serverAddress.split(":")
                        val host = parts.getOrElse(0) { "127.0.0.1" }
                        val port = parts.getOrElse(1) { "5100" }.toIntOrNull() ?: 5100
                        client.connect(host, port, channelName)
                    }
                },
                modifier = Modifier.fillMaxWidth(),
                colors = if (isConnected || isConnecting) {
                    ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)
                } else {
                    ButtonDefaults.buttonColors()
                }
            ) {
                Icon(
                    if (isConnected || isConnecting) Icons.Default.Stop else Icons.Default.PlayArrow,
                    contentDescription = null,
                    modifier = Modifier.size(20.dp)
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    when {
                        isConnecting -> "Connecting..."
                        isConnected -> "Disconnect"
                        else -> "Connect"
                    }
                )
            }

            HorizontalDivider()

            // --- Status card ---
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.surfaceVariant
                )
            ) {
                Column(
                    modifier = Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    // Connection status
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            Icons.Default.Circle,
                            contentDescription = null,
                            tint = when (state.status) {
                                SolunaClient.Status.CONNECTED -> Color(0xFF4CAF50)
                                SolunaClient.Status.CONNECTING -> Color(0xFFFFC107)
                                SolunaClient.Status.ERROR -> Color(0xFFF44336)
                                SolunaClient.Status.DISCONNECTED -> Color.Gray
                            },
                            modifier = Modifier.size(12.dp)
                        )
                        Spacer(Modifier.width(8.dp))
                        Text(
                            text = when (state.status) {
                                SolunaClient.Status.CONNECTED -> "Connected"
                                SolunaClient.Status.CONNECTING -> "Connecting..."
                                SolunaClient.Status.ERROR -> "Error"
                                SolunaClient.Status.DISCONNECTED -> "Disconnected"
                            },
                            style = MaterialTheme.typography.bodyLarge,
                            fontWeight = FontWeight.Medium
                        )
                    }

                    // Channel
                    if (state.channel.isNotEmpty()) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                Icons.Default.Radio,
                                contentDescription = null,
                                modifier = Modifier.size(16.dp),
                                tint = MaterialTheme.colorScheme.primary
                            )
                            Spacer(Modifier.width(8.dp))
                            Text("Channel: ${state.channel}")
                        }
                    }

                    // Members
                    if (state.memberCount > 0) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                Icons.Default.People,
                                contentDescription = null,
                                modifier = Modifier.size(16.dp),
                                tint = MaterialTheme.colorScheme.primary
                            )
                            Spacer(Modifier.width(8.dp))
                            Text("Members: ${state.memberCount}")
                        }
                    }

                    // Packets received
                    if (state.packetsReceived > 0) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                Icons.Default.GraphicEq,
                                contentDescription = null,
                                modifier = Modifier.size(16.dp),
                                tint = MaterialTheme.colorScheme.primary
                            )
                            Spacer(Modifier.width(8.dp))
                            Text("Packets: ${state.packetsReceived}")
                        }
                    }

                    // Wallet balance
                    if (state.walletBalance.isNotEmpty()) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                Icons.Default.AccountBalanceWallet,
                                contentDescription = null,
                                modifier = Modifier.size(16.dp),
                                tint = MaterialTheme.colorScheme.tertiary
                            )
                            Spacer(Modifier.width(8.dp))
                            Text(
                                "Balance: ${state.walletBalance}",
                                color = MaterialTheme.colorScheme.tertiary,
                                fontWeight = FontWeight.Medium
                            )
                        }
                    }

                    // Error
                    if (state.errorMessage != null) {
                        Text(
                            text = state.errorMessage!!,
                            color = MaterialTheme.colorScheme.error,
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                }
            }

            // --- Action buttons ---
            if (isConnected) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    OutlinedButton(
                        onClick = { client.requestWallet() },
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(Icons.Default.AccountBalanceWallet, contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Wallet")
                    }

                    Button(
                        onClick = { showTipDialog = true },
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(Icons.Default.Favorite, contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Tip")
                    }

                    OutlinedButton(
                        onClick = { client.requestMembers() },
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(Icons.Default.People, contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Members")
                    }
                }
            }

            Spacer(Modifier.weight(1f))

            // --- Footer ---
            Text(
                text = "OSTP/RTP - 48kHz/32bit stereo",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.align(Alignment.CenterHorizontally)
            )
        }
    }

    // --- Tip dialog ---
    if (showTipDialog) {
        AlertDialog(
            onDismissRequest = { showTipDialog = false },
            title = { Text("Send Tip") },
            text = {
                OutlinedTextField(
                    value = tipAmount,
                    onValueChange = { tipAmount = it },
                    label = { Text("Amount (USD)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    prefix = { Text("$") }
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        client.sendTip(tipAmount)
                        showTipDialog = false
                    }
                ) { Text("Send") }
            },
            dismissButton = {
                TextButton(onClick = { showTipDialog = false }) { Text("Cancel") }
            }
        )
    }
}

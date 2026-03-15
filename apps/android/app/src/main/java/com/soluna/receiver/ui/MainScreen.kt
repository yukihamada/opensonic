package com.soluna.receiver.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.soluna.receiver.MicTransmitter
import com.soluna.receiver.SolunaClient

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(
    client: SolunaClient,
    micTransmitter: MicTransmitter,
    onRequestMicPermission: (() -> Unit) -> Unit
) {
    val state by client.state.collectAsState()
    val txLevel by micTransmitter.level.collectAsState()
    val focusManager = LocalFocusManager.current

    var serverAddress by remember { mutableStateOf("relay.solun.art:5100") }
    var channelName by remember { mutableStateOf("default") }
    var password by remember { mutableStateOf("") }
    var showPassword by remember { mutableStateOf(false) }
    var tipAmount by remember { mutableStateOf("1.00") }
    var showTipDialog by remember { mutableStateOf(false) }

    val isConnected = state.status == SolunaClient.Status.CONNECTED
    val isConnecting = state.status == SolunaClient.Status.CONNECTING
    val isTxActive = micTransmitter.isActive

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Soluna", fontWeight = FontWeight.Bold) },
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
                .padding(16.dp)
                .verticalScroll(rememberScrollState()),
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
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next)
            )

            OutlinedTextField(
                value = password,
                onValueChange = { password = it },
                label = { Text("Password (optional)") },
                singleLine = true,
                enabled = !isConnected && !isConnecting,
                modifier = Modifier.fillMaxWidth(),
                leadingIcon = { Icon(Icons.Default.Lock, contentDescription = null) },
                trailingIcon = {
                    IconButton(onClick = { showPassword = !showPassword }) {
                        Icon(
                            if (showPassword) Icons.Default.VisibilityOff else Icons.Default.Visibility,
                            contentDescription = null
                        )
                    }
                },
                visualTransformation = if (showPassword) VisualTransformation.None
                                       else PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { focusManager.clearFocus() })
            )

            // --- Connect / Disconnect button ---
            Button(
                onClick = {
                    focusManager.clearFocus()
                    if (isConnected || isConnecting) {
                        if (isTxActive) micTransmitter.stop()
                        client.disconnect()
                    } else {
                        val parts = serverAddress.split(":")
                        val host = parts.getOrElse(0) { "127.0.0.1" }
                        val port = parts.getOrElse(1) { "5100" }.toIntOrNull() ?: 5100
                        client.connect(host, port, channelName, password)
                    }
                },
                modifier = Modifier.fillMaxWidth(),
                colors = if (isConnected || isConnecting)
                    ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)
                else ButtonDefaults.buttonColors()
            ) {
                Icon(
                    if (isConnected || isConnecting) Icons.Default.Stop else Icons.Default.PlayArrow,
                    contentDescription = null,
                    modifier = Modifier.size(20.dp)
                )
                Spacer(Modifier.width(8.dp))
                Text(when {
                    isConnecting -> "Connecting..."
                    isConnected -> "Disconnect"
                    else -> "Connect & Listen"
                })
            }

            // --- TX (Mic) button — only shown when connected ---
            if (isConnected) {
                Button(
                    onClick = {
                        if (isTxActive) {
                            micTransmitter.stop()
                        } else {
                            onRequestMicPermission { micTransmitter.start() }
                        }
                    },
                    modifier = Modifier.fillMaxWidth(),
                    colors = if (isTxActive)
                        ButtonDefaults.buttonColors(containerColor = Color(0xFF7C3AED))
                    else
                        ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.secondary)
                ) {
                    Icon(
                        if (isTxActive) Icons.Default.MicOff else Icons.Default.Mic,
                        contentDescription = null,
                        modifier = Modifier.size(20.dp)
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(if (isTxActive) "Stop Transmitting" else "Transmit Mic")
                }
            }

            HorizontalDivider()

            // --- Level meters ---
            if (isConnected) {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    LevelMeter(
                        label = "RX",
                        level = state.rxLevel,
                        color = Color(0xFF4CAF50)
                    )
                    if (isTxActive) {
                        LevelMeter(
                            label = "TX",
                            level = txLevel,
                            color = Color(0xFF7C3AED)
                        )
                    }
                }
                HorizontalDivider()
            }

            // --- Status card ---
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
            ) {
                Column(
                    modifier = Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
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
                                SolunaClient.Status.CONNECTED -> if (isTxActive) "TX Active" else "Listening"
                                SolunaClient.Status.CONNECTING -> "Connecting..."
                                SolunaClient.Status.ERROR -> "Error"
                                SolunaClient.Status.DISCONNECTED -> "Disconnected"
                            },
                            style = MaterialTheme.typography.bodyLarge,
                            fontWeight = FontWeight.Medium
                        )
                    }

                    if (state.channel.isNotEmpty()) {
                        StatusRow(Icons.Default.Radio, "Channel: ${state.channel}")
                    }
                    if (state.memberCount > 0) {
                        StatusRow(Icons.Default.People, "Members: ${state.memberCount}")
                    }
                    if (state.packetsReceived > 0) {
                        StatusRow(Icons.Default.GraphicEq, "Packets RX: ${state.packetsReceived}")
                    }
                    if (state.walletBalance.isNotEmpty()) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(Icons.Default.AccountBalanceWallet,
                                 contentDescription = null,
                                 modifier = Modifier.size(16.dp),
                                 tint = MaterialTheme.colorScheme.tertiary)
                            Spacer(Modifier.width(8.dp))
                            Text("Balance: ${state.walletBalance}",
                                 color = MaterialTheme.colorScheme.tertiary,
                                 fontWeight = FontWeight.Medium)
                        }
                    }
                    if (state.errorMessage != null) {
                        Text(state.errorMessage!!,
                             color = MaterialTheme.colorScheme.error,
                             style = MaterialTheme.typography.bodySmall)
                    }
                }
            }

            // --- Action buttons ---
            if (isConnected) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    OutlinedButton(onClick = { client.requestWallet() }, modifier = Modifier.weight(1f)) {
                        Icon(Icons.Default.AccountBalanceWallet, contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Wallet")
                    }
                    Button(onClick = { showTipDialog = true }, modifier = Modifier.weight(1f)) {
                        Icon(Icons.Default.Favorite, contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Tip")
                    }
                    OutlinedButton(onClick = { client.requestMembers() }, modifier = Modifier.weight(1f)) {
                        Icon(Icons.Default.People, contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Members")
                    }
                }
            }

            Spacer(Modifier.weight(1f))

            Text(
                text = "OSTP/RTP · 48kHz · 32bit stereo",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.align(Alignment.CenterHorizontally)
            )
        }
    }

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
                TextButton(onClick = { client.sendTip(tipAmount); showTipDialog = false }) {
                    Text("Send")
                }
            },
            dismissButton = {
                TextButton(onClick = { showTipDialog = false }) { Text("Cancel") }
            }
        )
    }
}

@Composable
private fun LevelMeter(label: String, level: Float, color: Color) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(
            label,
            style = MaterialTheme.typography.labelSmall,
            modifier = Modifier.width(24.dp),
            color = color
        )
        Box(
            modifier = Modifier
                .weight(1f)
                .height(8.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(4.dp))
        ) {
            val fraction = level.coerceIn(0f, 1f)
            if (fraction > 0f) {
                Box(
                    modifier = Modifier
                        .fillMaxHeight()
                        .fillMaxWidth(fraction)
                        .background(color, RoundedCornerShape(4.dp))
                )
            }
        }
    }
}

@Composable
private fun StatusRow(icon: androidx.compose.ui.graphics.vector.ImageVector, text: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(icon, contentDescription = null,
             modifier = Modifier.size(16.dp),
             tint = MaterialTheme.colorScheme.primary)
        Spacer(Modifier.width(8.dp))
        Text(text)
    }
}

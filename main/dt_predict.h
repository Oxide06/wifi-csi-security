// Auto-generated Decision Tree for ESP32
// No calibration needed — runs immediately on boot

// Feature order: ['mean', 'var', 'std', 'max', 'min', 'range', 'energy', 'zcr', 'skew', 'kurt']

static float dt_predict(float *f) {
    if (f[3] <= 15.760387f) {
        if (f[3] <= 15.053270f) {
            if (f[5] <= 6.471191f) {
                return 1.0000f; // class=1
            } else {
                return 0.0000f; // class=0
            }
        } else {
            if (f[7] <= 0.450000f) {
                if (f[9] <= -1.084823f) {
                    if (f[8] <= -0.072589f) {
                        if (f[3] <= 15.694161f) {
                            return 0.0000f; // class=0
                        } else {
                            return 1.0000f; // class=1
                        }
                    } else {
                        if (f[6] <= 235.343575f) {
                            return 1.0000f; // class=1
                        } else {
                            return 0.0000f; // class=0
                        }
                    }
                } else {
                    return 1.0000f; // class=1
                }
            } else {
                return 0.0000f; // class=0
            }
        }
    } else {
        if (f[6] <= 251.284439f) {
            if (f[0] <= 15.705070f) {
                if (f[3] <= 15.943320f) {
                    if (f[3] <= 15.935471f) {
                        if (f[8] <= -0.716035f) {
                            if (f[6] <= 241.185173f) {
                                return 1.0000f; // class=1
                            } else {
                                return 0.0000f; // class=0
                            }
                        } else {
                            if (f[2] <= 0.132195f) {
                                if (f[6] <= 245.908821f) {
                                    return 1.0000f; // class=1
                                } else {
                                    return 0.0000f; // class=0
                                }
                            } else {
                                return 0.0000f; // class=0
                            }
                        }
                    } else {
                        return 1.0000f; // class=1
                    }
                } else {
                    return 0.0000f; // class=0
                }
            } else {
                if (f[5] <= 0.697419f) {
                    if (f[0] <= 15.780678f) {
                        if (f[0] <= 15.722630f) {
                            return 0.0000f; // class=0
                        } else {
                            return 1.0000f; // class=1
                        }
                    } else {
                        return 0.0000f; // class=0
                    }
                } else {
                    if (f[4] <= 15.210729f) {
                        if (f[9] <= -1.692348f) {
                            return 1.0000f; // class=1
                        } else {
                            return 0.0000f; // class=0
                        }
                    } else {
                        return 1.0000f; // class=1
                    }
                }
            }
        } else {
            if (f[9] <= -1.184457f) {
                if (f[9] <= -1.199520f) {
                    return 0.0000f; // class=0
                } else {
                    return 1.0000f; // class=1
                }
            } else {
                return 0.0000f; // class=0
            }
        }
    }
}

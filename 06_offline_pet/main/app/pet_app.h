#pragma once

/* The decision layer. It owns the pet's mood and decides which recorded clip
 * answers which action. It never touches a sensor or a driver directly -
 * everything arrives as an app_event_t. */

void pet_app_start(void);

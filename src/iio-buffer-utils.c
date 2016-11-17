/*
 * Modified from industrialio buffer test code, and Lenovo Yoga (2 Pro) orientation helper
 * Copyright (c) 2008 Jonathan Cameron
 * Copyright (c) 2014 Peter F. Patel-Schneider
 * Copyright (c) 2014 Bastien Nocera <hadess@hadess.net>
 * Copyright (c) 2015 Elad Alfassa <elad@fedoraproject.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "iio-buffer-utils.h"

#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

/**
 * iio_channel_info - information about a given channel
 * @name: channel name
 * @scale: scale factor to be applied for conversion to si units
 * @offset: offset to be applied for conversion to si units
 * @index: the channel index in the buffer output
 * @bytes: number of bytes occupied in buffer output
 * @mask: a bit mask for the raw output
 * @is_signed: is the raw value stored signed
 * @enabled: is this channel enabled
 **/
struct iio_channel_info {
	char *name;
	char *generic_name;
	float scale;
	float offset;
	unsigned index;
	unsigned bytes;
	unsigned bits_used;
	unsigned shift;
	guint64 mask;
	unsigned be;
	unsigned is_signed;
	unsigned enabled;
	unsigned location;
};

static char *
iioutils_break_up_name (const char *name)
{
	char **items, *ret;
	guint i;

	items = g_strsplit (name, "_", -1);
	for (i = 0; items[i] != NULL; i++) {
		if (items[i + 1] == NULL) {
			g_clear_pointer (&items[i], g_free);
			break;
		}
	}

	ret = g_strjoinv ("_", items);
	g_strfreev (items);

	return ret;
}

/**
 * iioutils_get_type() - find and process _type attribute data
 * @is_signed: output whether channel is signed
 * @bytes: output how many bytes the channel storage occupies
 * @mask: output a bit mask for the raw data
 * @be: big endian
 * @device_dir: the iio device directory
 * @name: the channel name
 **/
static gboolean
iioutils_get_type (unsigned   *is_signed,
		   unsigned   *bytes,
		   unsigned   *bits_used,
		   unsigned   *shift,
		   guint64    *mask,
		   unsigned   *be,
		   const char *device_dir,
		   const char *name,
		   const char *generic_name)
{
	int ret;
	char *builtname;
	char *filename;
	char signchar, endianchar;
	unsigned padint;
	FILE *sysfsfp;

	builtname = g_strdup_printf ("%s_type", name);
	filename = g_build_filename (device_dir, "scan_elements", builtname, NULL);
	g_free (builtname);

	sysfsfp = fopen (filename, "r");
	if (sysfsfp == NULL) {
		builtname = g_strdup_printf ("%s_type", generic_name);
		filename = g_build_filename (device_dir, "scan_elements", builtname, NULL);
		g_free (builtname);

		sysfsfp = fopen (filename, "r");
		if (sysfsfp == NULL) {
			g_free (filename);
			return FALSE;
		}
	}

	ret = fscanf (sysfsfp,
		      "%ce:%c%u/%u>>%u",
		      &endianchar,
		      &signchar,
		      bits_used,
		      &padint, shift);

	if (ret < 0) {
		g_warning ("Failed to pass scan type description for %s", filename);
		fclose (sysfsfp);
		g_free (filename);
		return FALSE;
	}
	fclose (sysfsfp);

	*be = (endianchar == 'b');
	*bytes = padint / 8;
	if (*bits_used == 64)
		*mask = ~0;
	else
		*mask = (1 << *bits_used) - 1;
	if (signchar == 's')
		*is_signed = 1;
	else
		*is_signed = 0;

	g_debug ("Got type for %s: is signed: %d, bytes: %d, bits_used: %d, shift: %d, mask: 0x%" G_GUINT64_FORMAT ", be: %d",
		 name, *is_signed, *bytes, *bits_used, *shift, *mask, *be);

	g_free (filename);

	return TRUE;
}

static int
iioutils_get_param_float (float      *output,
			  const char *param_name,
			  const char *device_dir,
			  const char *name,
			  const char *generic_name)
{
	FILE *sysfsfp;
	char *builtname, *filename;
	int ret = 0;

	builtname = g_strdup_printf ("%s_%s", name, param_name);
	filename = g_build_filename (device_dir, builtname, NULL);
	g_free (builtname);

	sysfsfp = fopen (filename, "r");
	if (sysfsfp) {
		fscanf (sysfsfp, "%f", output);
		fclose (sysfsfp);
		g_free (filename);
		return 0;
	}

	g_free (filename);

	builtname = g_strdup_printf ("%s_%s", generic_name, param_name);
	filename = g_build_filename (device_dir, builtname, NULL);
	g_free (builtname);

	sysfsfp = fopen (filename, "r");
	if (sysfsfp) {
		fscanf (sysfsfp, "%f", output);
		fclose (sysfsfp);
	} else {
		ret = -errno;
		g_warning ("Failed to read float from %s", filename);
	}

	g_free (filename);

	return ret;
}

static void
channel_info_free (iio_channel_info *ci)
{
	g_free (ci->name);
	g_free (ci->generic_name);
	g_free (ci);
}

static int
compare_channel_index (gconstpointer a, gconstpointer b)
{
	const iio_channel_info *info_1 = *(iio_channel_info **) a;
	const iio_channel_info *info_2 = *(iio_channel_info **) b;

	return (int) (info_1->index - info_2->index);
}

/* build_channel_array() - function to figure out what channels are present */
static iio_channel_info **
build_channel_array (const char        *device_dir,
		     int               *counter)
{
	GDir *dp;
	FILE *sysfsfp;
	int ret;
	const char *name;
	char *scan_el_dir;
	GPtrArray *array;
	iio_channel_info **ret_array;
	int i;

	*counter = 0;
	scan_el_dir = g_build_filename (device_dir, "scan_elements", NULL);

	dp = g_dir_open (scan_el_dir, 0, NULL);
	if (dp == NULL) {
		ret = -errno;
		g_debug ("Could not open scan_elements dir '%s'", scan_el_dir);
		g_free (scan_el_dir);
		return NULL;
	}

	array = g_ptr_array_new_full (0, (GDestroyNotify) channel_info_free);

	while ((name = g_dir_read_name (dp)) != NULL) {
		if (g_str_has_suffix (name, "_en")) {
			char *filename, *index_name;
			iio_channel_info *current;

			filename = g_build_filename (scan_el_dir, name, NULL);
			sysfsfp = fopen (filename, "r");
			if (sysfsfp == NULL) {
				g_debug ("Could not open scan_elements file '%s'", filename);
				g_free (filename);
				continue;
			}
			fscanf (sysfsfp, "%d", &ret);
			fclose (sysfsfp);
			if (!ret) {
				g_debug ("Could not read from scan_elements file '%s'", filename);
				g_free (filename);
				continue;
			}
			g_free (filename);

			current = g_new0 (iio_channel_info, 1);

			current->scale = 1.0;
			current->offset = 0;
			current->name = g_strndup (name, strlen(name) - strlen("_en"));
			current->generic_name = iioutils_break_up_name (current->name);
			if (g_strcmp0(current->generic_name, "in_rot_from_north_magnetic_tilt") == 0) {
				current->generic_name = g_strdup ("in_rot");
			}

			index_name = g_strdup_printf ("%s_index", current->name);
			filename = g_build_filename (scan_el_dir, index_name, NULL);
			g_free (index_name);

			sysfsfp = fopen (filename, "r");
			fscanf (sysfsfp, "%u", &current->index);
			fclose (sysfsfp);
			g_free (filename);

			/* Find the scale */
			ret = iioutils_get_param_float (&current->scale,
							"scale",
							device_dir,
							current->name,
							current->generic_name);
			if ((ret < 0) && (ret != -ENOENT))
				goto error;

			ret = iioutils_get_param_float (&current->offset,
							"offset",
							device_dir,
							current->name,
							current->generic_name);
			if ((ret < 0) && (ret != -ENOENT))
				goto error;

			ret = iioutils_get_type (&current->is_signed,
						 &current->bytes,
						 &current->bits_used,
						 &current->shift,
						 &current->mask,
						 &current->be,
						 device_dir,
						 current->name,
						 current->generic_name);

			if (!ret) {
				g_warning ("Could not parse name %s, generic name %s",
					   current->name, current->generic_name);
			} else {
				g_ptr_array_add (array, current);
			}
		}
	}
	g_dir_close (dp);
	g_free (scan_el_dir);

	g_ptr_array_sort (array, compare_channel_index);

	*counter = array->len;
	ret_array = (iio_channel_info **) g_ptr_array_free (array, FALSE);

	for (i = 0; i < *counter; i++) {
		iio_channel_info *ci = ret_array[i];

		g_debug ("Built channel array for %s: index: %d, is signed: %d, bytes: %d, bits_used: %d, shift: %d, mask: 0x%" G_GUINT64_FORMAT ", be: %d",
			 ci->name, ci->index, ci->is_signed, ci->bytes, ci->bits_used, ci->shift, ci->mask, ci->be);
	}

	return ret_array;

error:
	g_ptr_array_free (array, TRUE);
	g_dir_close (dp);
	g_free (scan_el_dir);
	return NULL;
}

static int
_write_sysfs_int (const char *filename,
		  const char *basedir,
		  int         val,
		  int         verify,
		  int         type,
		  int         val2)
{
	int ret = 0;
	FILE *sysfsfp;
	int test;
	char *temp;
	temp = g_build_filename (basedir, filename, NULL);
	sysfsfp = fopen(temp, "w");
	if (sysfsfp == NULL) {
		g_warning ("Could not open for write '%s'", temp);
		ret = -errno;
		goto error_free;
	}
	if (type)
		fprintf(sysfsfp, "%d %d", val, val2);
	else
		fprintf(sysfsfp, "%d", val);

	fclose(sysfsfp);
	if (verify) {
		sysfsfp = fopen(temp, "r");
		if (sysfsfp == NULL) {
			g_warning ("Could not open for read '%s'", temp);
			ret = -errno;
			goto error_free;
		}
		fscanf(sysfsfp, "%d", &test);
		if (test != val) {
			g_warning ("Possible failure in int write %d to %s",
				   val, temp);
			ret = -1;
		}
		fclose(sysfsfp);
	}
error_free:
	g_free (temp);
	return ret;
}

static int write_sysfs_int(const char *filename, const char *basedir, int val) {
	return _write_sysfs_int(filename, basedir, val, 0, 0, 0);
}

static int write_sysfs_int_and_verify(const char *filename, const char *basedir, int val) {
	return _write_sysfs_int(filename, basedir, val, 1, 0, 0);
}

static int
_write_sysfs_string (const char *filename,
		     const char *basedir,
		     const char *val,
		     int         verify)
{
	int ret = 0;
	FILE *sysfsfp;
	char *temp;

	temp = g_build_filename (basedir, filename, NULL);
	sysfsfp = fopen (temp, "w");
	if (sysfsfp == NULL) {
		ret = -errno;
		goto error_free;
	}
	fprintf(sysfsfp, "%s", val);
	fclose(sysfsfp);

	/* Verify? */
	if (!verify)
		goto error_free;
	sysfsfp = fopen(temp, "r");
	if (sysfsfp == NULL) {
		ret = -errno;
		goto error_free;
	}
	fscanf(sysfsfp, "%s", temp);
	if (strcmp(temp, val) != 0) {
		g_warning ("Possible failure in string write of %s Should be %s written to %s\\%s\n",
			   temp, val, basedir, filename);
		ret = -1;
	}
	fclose(sysfsfp);

error_free:
	g_free(temp);

	return ret;
}

/**
 * write_sysfs_string_and_verify() - string write, readback and verify
 * @filename: name of file to write to
 * @basedir: the sysfs directory in which the file is to be found
 * @val: the string to write
 **/
static int write_sysfs_string_and_verify(const char *filename, const char *basedir, const char *val) {
	return _write_sysfs_string(filename, basedir, val, 1);
}

static int write_sysfs_string(const char *filename, const char *basedir, const char *val) {
	return _write_sysfs_string(filename, basedir, val, 0);
}


/**
 * size_from_channelarray() - calculate the storage size of a scan
 * @channels:           the channel info array
 * @num_channels:       number of channels
 *
 * Has the side effect of filling the channels[i].location values used
 * in processing the buffer output.
 **/
static int
size_from_channelarray (iio_channel_info **channels,
			int                num_channels)
{
	int bytes = 0;
	int i = 0;
	while (i < num_channels) {
		if (bytes % channels[i]->bytes == 0)
			channels[i]->location = bytes;
		else
			channels[i]->location = bytes - bytes % channels[i]->bytes
				+ channels[i]->bytes;
		bytes = channels[i]->location + channels[i]->bytes;
		i++;
	}
	return bytes;
}

/**
 * process_scan_1() - get an integer value for a particular channel
 * @data:               pointer to the start of the scan
 * @buffer_data:        Buffer information
 * ch_name:		name of channel to get
 * ch_val:		value for the channel
 * ch_scale:		scale for the channel
 * ch_present:		whether the channel is present
 **/
void
process_scan_1 (char              *data,
    BufferDrvData        *buffer_data,
		const char        *ch_name,
		int               *ch_val,
		gdouble           *ch_scale,
		gboolean          *ch_present)
{
	int k;
	for (k = 0; k < buffer_data->channels_count; k++) {
		if (strcmp (buffer_data->channels[k]->name, ch_name) != 0)
			continue;

		g_debug ("process_scan_1: channel_index: %d, chan_name: %s, channel_data_index: %d location: %d",
			 k, buffer_data->channels[k]->name, buffer_data->channels[k]->index, buffer_data->channels[k]->location);

		switch (buffer_data->channels[k]->bytes) {
			/* only a few cases implemented so far */
		case 4:
			if (!buffer_data->channels[k]->is_signed) {
				guint32 val = *(guint32 *) (data + buffer_data->channels[k]->location);
				val = val >> buffer_data->channels[k]->shift;
				if (buffer_data->channels[k]->bits_used < 32) val &= ((guint32) 1 << buffer_data->channels[k]->bits_used) - 1;
				*ch_val = (int) val;
				*ch_present = TRUE;
			} else {
				gint32 val = *(gint32 *) (data + buffer_data->channels[k]->location);
				val = val >> buffer_data->channels[k]->shift;
				if (buffer_data->channels[k]->bits_used < 32) val &= ((guint32) 1 << buffer_data->channels[k]->bits_used) - 1;
				val = (gint32) (val << (32 - buffer_data->channels[k]->bits_used)) >> (32 - buffer_data->channels[k]->bits_used);
				*ch_val = (int) val;
				if (buffer_data->channels[k]->scale)
					*ch_scale = buffer_data->channels[k]->scale;
				else
					*ch_scale = 1.0;
				*ch_present = TRUE;
			}
			break;
		case 2:
		case 8:
			g_error ("Process %d bytes channels not supported yet", buffer_data->channels[k]->bytes);
		default:
			g_assert_not_reached ();
			break;
		}
	}

	if (!*ch_present)
		g_warning ("IIO channel '%s' could not be found", ch_name);
}

/**
 * enable_sensors: enable all the sensors in a device
 * @device_dir: the IIO device directory in sysfs
 * @
 **/
static gboolean
enable_sensors (GUdevDevice *dev,
                int          enable)
{
	GDir *dir;
	char *device_dir;
	const char *name;
	gboolean ret = FALSE;

	device_dir = g_build_filename (g_udev_device_get_sysfs_path (dev), "scan_elements", NULL);
	dir = g_dir_open (device_dir, 0, NULL);
	if (!dir) {
		g_free (device_dir);
		return FALSE;
	}

	while ((name = g_dir_read_name (dir))) {
		char *path;

		if (g_str_has_suffix (name, "_en") == FALSE)
			continue;

		/* Already enabled? */
		path = g_strdup_printf ("scan_elements/%s", name);
		if (g_udev_device_get_sysfs_attr_as_boolean (dev, path)) {
			ret = TRUE;
			g_free (path);
			continue;
		}
		g_free (path);

		/* Enable */
		if (write_sysfs_int (name, device_dir, enable) < 0) {
			g_warning ("Could not enable sensor %s/%s", device_dir, name);
			continue;
		}

		ret = TRUE;
		g_debug ("Enabled sensor %s/%s", device_dir, name);
	}
	g_dir_close (dir);
	g_free (device_dir);

	if (!ret) {
		g_warning ("Failed to enable any sensors for device '%s'",
			   g_udev_device_get_sysfs_path (dev));
	}

	return ret;
}

static gboolean
enable_ring_buffer (BufferDrvData *data)
{
	int ret;

	/* Setup ring buffer parameters */
	ret = write_sysfs_int("buffer/length", data->dev_dir_name, 128);
	if (ret < 0) {
		g_warning ("Failed to set ring buffer length for %s", data->dev_dir_name);
		return FALSE;
	}
	/* Enable the buffer */
	ret = write_sysfs_int_and_verify("buffer/enable", data->dev_dir_name, 1);
	if (ret < 0) {
		g_warning ("Unable to enable ring buffer for %s", data->dev_dir_name);
		return FALSE;
	}

	return TRUE;
}

static void
disable_ring_buffer (BufferDrvData *data)
{
	/* Stop the buffer */
	write_sysfs_int ("buffer/enable", data->dev_dir_name, 0);

	/* Disconnect the trigger - just write a dummy name. */
	write_sysfs_string ("trigger/current_trigger", data->dev_dir_name, "NULL");
}

static gboolean
enable_trigger (BufferDrvData *data)
{
	int ret;

	/* Set the device trigger to be the data ready trigger */
	ret = write_sysfs_string_and_verify("trigger/current_trigger",
			data->dev_dir_name, data->trigger_name);
	if (ret < 0) {
		g_warning ("Failed to write current_trigger file %s", g_strerror(-ret));
		return FALSE;
	}

	return TRUE;
}

static gboolean
build_channels (BufferDrvData *data)
{
	/* Parse the files in scan_elements to identify what channels are present */
	data->channels = build_channel_array (data->dev_dir_name, &(data->channels_count));
	if (data->channels == NULL) {
		g_warning ("Problem reading scan element information: %s", data->dev_dir_name);
		return FALSE;
	}
	data->scan_size = size_from_channelarray (data->channels, data->channels_count);
	return TRUE;
}

void
buffer_drv_data_free (BufferDrvData *buffer_data)
{
	int i;

	if (buffer_data == NULL)
		return;

	enable_sensors (buffer_data->device, 0);
	g_clear_object (&buffer_data->device);

	disable_ring_buffer (buffer_data);

	g_free (buffer_data->trigger_name);

	for (i = 0; i < buffer_data->channels_count; i++)
		channel_info_free (buffer_data->channels[i]);
	g_free (buffer_data->channels);
}

BufferDrvData *
buffer_drv_data_new (GUdevDevice *device,
		     const char  *trigger_name)
{
	BufferDrvData *buffer_data;

	buffer_data = g_new0 (BufferDrvData, 1);
	buffer_data->dev_dir_name = g_udev_device_get_sysfs_path (device);
	buffer_data->trigger_name = g_strdup (trigger_name);
	buffer_data->device = g_object_ref (device);

	if (!enable_sensors (device, 1) ||
	    !enable_trigger (buffer_data) ||
	    !enable_ring_buffer (buffer_data) ||
	    !build_channels (buffer_data)) {
		buffer_drv_data_free (buffer_data);
		return NULL;
	}

	return buffer_data;
}


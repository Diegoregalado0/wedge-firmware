/** @type {import('next').NextConfig} */
const nextConfig = {
  // The device polls over plain HTTPS with a bearer token and parses the body
  // with cJSON, so responses must stay small and must never be compressed
  // into something an embedded client has to negotiate.
  poweredByHeader: false,
};

export default nextConfig;
